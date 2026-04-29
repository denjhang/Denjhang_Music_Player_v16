// sn76489_dump.cpp - Standalone SN76489 VGM command dumper
// Compile: g++ -o sn76489_dump sn76489_dump.cpp -lz
// Usage:   sn76489_dump <file.vgm>
// Prints all SN76489 register writes and reconstructed channel state.

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

struct SN_State {
    uint16_t period[4]; // tone0..2, noise
    uint8_t  vol[4];    // 0=max, 15=silent
    int      latch;     // latched register index
    SN_State() { memset(period,0,sizeof(period)); memset(vol,0xFF,sizeof(vol)); latch=0; }
};

static const char* note_name(double freq) {
    static char buf[32];
    if (freq <= 0) { snprintf(buf,sizeof(buf),"---"); return buf; }
    // A4=440Hz, MIDI note 69
    double midi = 69.0 + 12.0 * log2(freq / 440.0);
    int n = (int)(midi + 0.5);
    const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    int oct = n/12 - 1;
    snprintf(buf,sizeof(buf),"%s%d(%.1fHz)",names[n%12],oct,freq);
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <file.vgm>\n", argv[0]); return 1; }

    // Load file
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> raw(fsize);
    fread(raw.data(), 1, fsize, f);
    fclose(f);

    // Decompress VGZ if needed
    std::vector<uint8_t> data;
    if (raw.size() >= 2 && raw[0] == 0x1F && raw[1] == 0x8B) {
        // gzip compressed
        uLongf destLen = fsize * 8;
        data.resize(destLen);
        // Use zlib inflate with gzip wrapper
        z_stream zs = {};
        inflateInit2(&zs, 16+MAX_WBITS);
        zs.next_in  = raw.data();
        zs.avail_in = (uInt)raw.size();
        zs.next_out = data.data();
        zs.avail_out = (uInt)data.size();
        int r = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (r != Z_STREAM_END) { fprintf(stderr, "Decompress failed\n"); return 1; }
        data.resize(zs.total_out);
    } else {
        data = raw;
    }

    // Validate header
    if (data.size() < 0x40) { fprintf(stderr, "File too small\n"); return 1; }
    if (memcmp(data.data(), "Vgm ", 4) != 0) { fprintf(stderr, "Not a VGM file\n"); return 1; }

    uint32_t version  = read_le32(data.data() + 0x08);
    uint32_t eof_ofs  = read_le32(data.data() + 0x04);
    eof_ofs += 0x04; // relative to 0x04
    if (eof_ofs == 0x04 || eof_ofs > data.size()) eof_ofs = (uint32_t)data.size();

    uint32_t data_ofs;
    if (version >= 0x150 && data.size() >= 0x40) {
        uint32_t rel = read_le32(data.data() + 0x34);
        data_ofs = rel ? (0x34 + rel) : 0x40;
    } else {
        data_ofs = 0x40;
    }

    // GD3 offset
    uint32_t gd3_ofs = 0;
    if (data.size() >= 0x18) {
        uint32_t rel = read_le32(data.data() + 0x14);
        if (rel) gd3_ofs = 0x14 + rel;
    }
    uint32_t data_end = eof_ofs;
    if (gd3_ofs && gd3_ofs < data_end && gd3_ofs >= data_ofs)
        data_end = gd3_ofs;

    // SN76489 clock
    uint32_t sn_clock = 0;
    if (data.size() >= 0x0C) sn_clock = read_le32(data.data() + 0x0C);
    double sn_freq_divisor = (sn_clock > 0) ? (double)sn_clock : 3579545.0;

    printf("VGM version: %X.%02X\n", (version>>8)&0xFF, version&0xFF);
    printf("Data offset: 0x%X, End: 0x%X\n", data_ofs, data_end);
    printf("SN76489 clock: %u Hz\n", sn_clock);
    printf("\n--- SN76489 Commands (cmd 0x50) ---\n");
    printf("%-8s %-6s %-20s  State: T0_period T1_period T2_period N_period | T0_vol T1_vol T2_vol N_vol\n",
           "Tick", "Byte", "Action");
    printf("%s\n", std::string(100,'-').c_str());

    SN_State st;
    uint32_t tick = 0;
    uint32_t pos  = data_ofs;
    int sn_count  = 0;

    while (pos < data_end) {
        uint8_t cmd = data[pos];

        if (cmd == 0x66) break; // end of stream

        if (cmd == 0x50) {
            // SN76489 write (chip 0)
            uint8_t d = data[pos+1];
            char action[64];
            if (d & 0x80) {
                // LATCH byte
                st.latch = (d >> 4) & 0x07;
                int ch   = st.latch >> 1;
                int type = st.latch & 1;
                if (type == 0) {
                    // tone/noise period low 4 bits
                    st.period[ch] = (st.period[ch] & 0x3F0) | (d & 0x0F);
                    snprintf(action, sizeof(action), "LATCH ch%d PERIOD_LO=%X period=%d", ch, d&0xF, st.period[ch]);
                } else {
                    // volume
                    st.vol[ch] = d & 0x0F;
                    snprintf(action, sizeof(action), "LATCH ch%d VOL=%d (%s)", ch, st.vol[ch], st.vol[ch]==15?"silent":"on");
                }
            } else {
                // DATA byte
                int ch   = st.latch >> 1;
                int type = st.latch & 1;
                if (type == 0) {
                    // period high 6 bits
                    st.period[ch] = ((d & 0x3F) << 4) | (st.period[ch] & 0x0F);
                    double freq = (st.period[ch] > 0 && ch < 3)
                        ? sn_freq_divisor / (32.0 * st.period[ch])
                        : 0.0;
                    snprintf(action, sizeof(action), "DATA  ch%d PERIOD_HI=%X period=%d %s",
                             ch, d&0x3F, st.period[ch], note_name(freq));
                } else {
                    snprintf(action, sizeof(action), "DATA  (vol, unexpected)");
                }
            }
            printf("t=%-8u 0x%02X  %-40s | %4d %4d %4d %4d | %2d %2d %2d %2d\n",
                   tick, d, action,
                   st.period[0], st.period[1], st.period[2], st.period[3],
                   st.vol[0], st.vol[1], st.vol[2], st.vol[3]);
            sn_count++;
            pos += 2;
            continue;
        }

        // Wait / flow commands
        if (cmd == 0x61) { tick += read_le16(data.data()+pos+1); pos += 3; continue; }
        if (cmd == 0x62) { tick += 735; pos += 1; continue; }
        if (cmd == 0x63) { tick += 882; pos += 1; continue; }
        if (cmd >= 0x70 && cmd <= 0x7F) { tick += (cmd&0xF)+1; pos += 1; continue; }
        if (cmd == 0x67) { // data block
            uint32_t blen = read_le32(data.data()+pos+3);
            pos += 7 + blen; continue;
        }
        if (cmd == 0x68) { pos += 12; continue; } // PCM RAM write
        if (cmd >= 0x80 && cmd <= 0x8F) { tick += cmd&0xF; pos += 1; continue; } // YM2612 DAC
        if (cmd == 0xE0) { pos += 5; continue; } // PCM seek

        // Generic: skip by command table
        // 1-byte commands: 0x60-0x6F (except 0x61,0x62,0x63,0x67,0x68), 0x70-0x8F
        // 2-byte: 0x30-0x4F, 0x4F, 0x50
        // 3-byte: 0x51-0x5F, 0xA0-0xAF
        // 4-byte: 0xC0-0xCF, 0xD0-0xDF
        // 5-byte: 0xE1-0xFF
        if (cmd >= 0x30 && cmd <= 0x3F) { pos += 2; continue; }
        if (cmd >= 0x40 && cmd <= 0x4E) { pos += 3; continue; }
        if (cmd == 0x4F) { pos += 2; continue; }
        if (cmd >= 0x51 && cmd <= 0x5F) { pos += 3; continue; }
        if (cmd >= 0xA0 && cmd <= 0xAF) { pos += 3; continue; }
        if (cmd >= 0xB0 && cmd <= 0xBF) { pos += 3; continue; }
        if (cmd >= 0xC0 && cmd <= 0xCF) { pos += 4; continue; }
        if (cmd >= 0xD0 && cmd <= 0xDF) { pos += 4; continue; }
        if (cmd >= 0xE1 && cmd <= 0xFF) { pos += 5; continue; }
        // Unknown: stop
        fprintf(stderr, "Unknown cmd 0x%02X at pos 0x%X, stopping.\n", cmd, pos);
        break;
    }

    printf("\nTotal SN76489 writes: %d\n", sn_count);
    printf("Final state:\n");
    for (int ch = 0; ch < 3; ch++) {
        double freq = st.period[ch] > 0 ? sn_freq_divisor/(32.0*st.period[ch]) : 0.0;
        printf("  T%d: period=%d freq=%.1fHz vol=%d (%s)\n",
               ch, st.period[ch], freq, st.vol[ch], st.vol[ch]==15?"silent":"active");
    }
    printf("  Noise: period=%d vol=%d (%s)\n",
           st.period[3], st.vol[3], st.vol[3]==15?"silent":"active");
    return 0;
}
