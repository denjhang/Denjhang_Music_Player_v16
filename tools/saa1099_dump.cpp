// saa1099_dump.cpp - Standalone SAA1099 VGM command dumper
// Compile: g++ -O2 -o saa1099_dump saa1099_dump.cpp -lz -lm
// Usage:   saa1099_dump <file.vgm>

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>
#include <vector>
#include <string>
#include <math.h>

static uint32_t read_le32(const uint8_t* d) {
    return d[0] | (d[1]<<8) | (d[2]<<16) | (d[3]<<24);
}
static uint16_t read_le16(const uint8_t* d) {
    return d[0] | (d[1]<<8);
}

// SAA1099 register map:
//  0x00-0x05: Channel amplitude (bits7:4=left, bits3:0=right)
//  0x08-0x0D: Channel frequency (8-bit)
//  0x10-0x14: Channel octave/freq_en (0x10=ch1:0 oct, 0x14=freq enable)
//  0x15:      Noise enable
//  0x16:      Noise generator control
//  0x18-0x19: Envelope generators
//  0x1C:      Master enable (bit0)

struct SAA_State {
    uint8_t regs[0x20];
    SAA_State() { memset(regs, 0, sizeof(regs)); }
};

static const char* note_name(double freq) {
    static char buf[40];
    if (freq <= 0) { snprintf(buf, sizeof(buf), "---"); return buf; }
    double midi = 69.0 + 12.0 * log2(freq / 440.0);
    int n = (int)(midi + 0.5);
    if (n < 0 || n > 127) { snprintf(buf, sizeof(buf), "%.1fHz", freq); return buf; }
    const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    int oct = n/12 - 1;
    snprintf(buf, sizeof(buf), "%s%d(%.1fHz)", names[n%12], oct, freq);
    return buf;
}

// SAA1099 frequency formula (from libvgm/yasp):
// clk2div512 = (clock + 128) / 256
// freq = clk2div512 * (1 << octave) / (511 - freq_val)
static double saa_freq(const SAA_State& st, int ch, double clock) {
    uint8_t freq_val = st.regs[0x08 + ch];
    int octave_reg = ch / 2;
    int octave;
    if (ch & 1)
        octave = (st.regs[0x10 + octave_reg] >> 4) & 0x07;
    else
        octave = st.regs[0x10 + octave_reg] & 0x07;
    if (freq_val >= 511) return 0.0;
    double clk2div512 = (clock + 128.0) / 256.0;
    return clk2div512 * (1 << octave) / (511.0 - freq_val);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <file.vgm>\n", argv[0]); return 1; }

    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> raw(fsize);
    fread(raw.data(), 1, fsize, f);
    fclose(f);

    std::vector<uint8_t> data;
    if (raw.size() >= 2 && raw[0] == 0x1F && raw[1] == 0x8B) {
        z_stream zs = {};
        inflateInit2(&zs, 16+MAX_WBITS);
        data.resize(fsize * 8);
        zs.next_in = raw.data(); zs.avail_in = (uInt)raw.size();
        zs.next_out = data.data(); zs.avail_out = (uInt)data.size();
        int r = inflate(&zs, Z_FINISH); inflateEnd(&zs);
        if (r != Z_STREAM_END) { fprintf(stderr, "Decompress failed\n"); return 1; }
        data.resize(zs.total_out);
    } else { data = raw; }

    if (data.size() < 0x40 || memcmp(data.data(), "Vgm ", 4) != 0) {
        fprintf(stderr, "Not a valid VGM file\n"); return 1;
    }

    uint32_t version = read_le32(data.data() + 0x08);
    uint32_t eof_ofs = read_le32(data.data() + 0x04) + 0x04;
    if (eof_ofs == 0x04 || eof_ofs > data.size()) eof_ofs = (uint32_t)data.size();

    uint32_t data_ofs;
    if (version >= 0x150) {
        uint32_t rel = read_le32(data.data() + 0x34);
        data_ofs = rel ? (0x34 + rel) : 0x40;
    } else { data_ofs = 0x40; }

    uint32_t gd3_ofs = 0;
    if (data.size() >= 0x18) {
        uint32_t rel = read_le32(data.data() + 0x14);
        if (rel) gd3_ofs = 0x14 + rel;
    }
    uint32_t data_end = eof_ofs;
    if (gd3_ofs && gd3_ofs < data_end && gd3_ofs >= data_ofs) data_end = gd3_ofs;

    double saa_clock = 8053400.0;
    if (version >= 0x151 && data.size() >= 0x80) {
        uint32_t clk = read_le32(data.data() + 0x7C);
        if (clk) saa_clock = (double)clk;
    }

    printf("VGM version: %X.%02X\n", (version>>8)&0xFF, version&0xFF);
    printf("Data: 0x%X - 0x%X\n", data_ofs, data_end);
    printf("SAA1099 clock: %.0f Hz\n\n", saa_clock);
    printf("--- SAA1099 Commands (cmd 0xBD) ---\n");
    printf("%-8s  Reg  Data  Description\n", "Tick");
    printf("%s\n", std::string(70, '-').c_str());

    SAA_State st;
    uint32_t tick = 0, pos = data_ofs;
    int saa_count = 0;

    while (pos < data_end) {
        uint8_t cmd = data[pos];
        if (cmd == 0x66) break;

        if (cmd == 0xBD) {
            uint8_t reg = data[pos+1];
            uint8_t val = data[pos+2];
            if (reg < 0x20) st.regs[reg] = val;

            char desc[80] = "";
            if (reg <= 0x05)
                snprintf(desc, sizeof(desc), "CH%d Amp L=%X R=%X", reg, (val>>4)&0xF, val&0xF);
            else if (reg >= 0x08 && reg <= 0x0D)
                snprintf(desc, sizeof(desc), "CH%d Freq=%02X", reg-8, val);
            else if (reg >= 0x10 && reg <= 0x12)
                snprintf(desc, sizeof(desc), "Octave CH%d=%d CH%d=%d", (reg-0x10)*2, val&7, (reg-0x10)*2+1, (val>>4)&7);
            else if (reg == 0x14)
                snprintf(desc, sizeof(desc), "FreqEn=0x%02X", val);
            else if (reg == 0x15)
                snprintf(desc, sizeof(desc), "NoiseEn=0x%02X", val);
            else if (reg == 0x16)
                snprintf(desc, sizeof(desc), "NoiseCTL=0x%02X", val);
            else if (reg == 0x18 || reg == 0x19)
                snprintf(desc, sizeof(desc), "Env%d=0x%02X", reg-0x18, val);
            else if (reg == 0x1C)
                snprintf(desc, sizeof(desc), "MasterEn=0x%02X (%s)", val, (val&1)?"ON":"OFF");

            printf("t=%-8u  %02X   %02X    %s\n", tick, reg, val, desc);
            saa_count++;
            pos += 3; continue;
        }

        if (cmd == 0x61) { tick += read_le16(data.data()+pos+1); pos += 3; continue; }
        if (cmd == 0x62) { tick += 735; pos++; continue; }
        if (cmd == 0x63) { tick += 882; pos++; continue; }
        if (cmd >= 0x70 && cmd <= 0x7F) { tick += (cmd&0xF)+1; pos++; continue; }
        if (cmd == 0x67) { uint32_t bl = read_le32(data.data()+pos+3); pos += 7+bl; continue; }
        if (cmd == 0x68) { pos += 12; continue; }
        if (cmd >= 0x80 && cmd <= 0x8F) { tick += cmd&0xF; pos++; continue; }
        if (cmd == 0xE0) { pos += 5; continue; }
        if (cmd >= 0x30 && cmd <= 0x3F) { pos += 2; continue; }
        if (cmd >= 0x40 && cmd <= 0x4E) { pos += 3; continue; }
        if (cmd == 0x4F) { pos += 2; continue; }
        if (cmd >= 0x50 && cmd <= 0x5F) { pos += 3; continue; }
        if (cmd >= 0xA0 && cmd <= 0xBF) { pos += 3; continue; }
        if (cmd >= 0xC0 && cmd <= 0xCF) { pos += 4; continue; }
        if (cmd >= 0xD0 && cmd <= 0xDF) { pos += 4; continue; }
        if (cmd >= 0xE1 && cmd <= 0xFF) { pos += 5; continue; }
        fprintf(stderr, "Unknown cmd 0x%02X at 0x%X, stopping.\n", cmd, pos);
        break;
    }

    printf("\nTotal SAA1099 writes: %d\n", saa_count);
    printf("\nFinal channel state:\n");
    uint8_t freq_en  = st.regs[0x14];
    uint8_t noise_en = st.regs[0x15];
    printf("  MasterEn=0x%02X  FreqEn=0x%02X  NoiseEn=0x%02X\n",
        st.regs[0x1C], freq_en, noise_en);
    for (int ch = 0; ch < 6; ch++) {
        uint8_t amp = st.regs[ch];
        uint8_t fv  = st.regs[0x08 + ch];
        int oct_reg = ch / 2;
        int oct = (ch&1) ? ((st.regs[0x10+oct_reg]>>4)&7) : (st.regs[0x10+oct_reg]&7);
        double freq = saa_freq(st, ch, saa_clock);
        bool fen = (freq_en >> ch) & 1;
        bool nen = (noise_en >> ch) & 1;
        printf("  CH%d: AmpL=%X AmpR=%X Freq=%02X Oct=%d => %.1fHz %s%s  %s\n",
            ch, (amp>>4)&0xF, amp&0xF, fv, oct, freq,
            fen?"[FREQ]":"", nen?"[NOISE]":"",
            ((amp>>4)|(amp&0xF)) ? note_name(freq) : "silent");
    }
    return 0;
}
