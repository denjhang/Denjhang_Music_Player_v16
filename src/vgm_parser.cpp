#include "vgm_parser.h"
#include "libvgm-modizer/player/vgmplayer.hpp" // For VGM_HEADER definition
#include "libvgm-modizer/utils/StrUtils.h"
#include "libvgm-modizer/emu/SoundDevs.h" // For DEVID_* constants
#include <string.h>
#include <vector>
#include <zlib.h>

// Helper functions from vgmplayer.cpp
INLINE UINT16 ReadLE16(const UINT8* data)
{
#ifdef VGM_LITTLE_ENDIAN
	return *(UINT16*)data;
#else
	return (data[0x01] << 8) | (data[0x00] << 0);
#endif
}

INLINE UINT32 ReadLE32(const UINT8* data)
{
#ifdef VGM_LITTLE_ENDIAN
	return	*(UINT32*)data;
#else
	return	(data[0x03] << 24) | (data[0x02] << 16) |
			(data[0x01] <<  8) | (data[0x00] <<  0);
#endif
}

INLINE UINT32 ReadRelOfs(const UINT8* data, UINT32 fileOfs)
{
	UINT32 ofs = ReadLE32(&data[fileOfs]);
	return ofs ? (fileOfs + ofs) : ofs;
}

// Command Info Table - using DEVID_* constants
static const VGM_COMMAND_INFO VGM_CMD_INFO[0x100] =
{
	{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0x00-0x07 - Invalid commands
	{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0x08-0x0F
	{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0x10-0x17
	{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0x18-0x1F
	{3, DEVID_AY8910, 0}, {2, 0, 0}, {3, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, // 0x20-0x27: 20=AY8910 22=K005289(2-byte, not in modizer)
	{2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, // 0x28-0x2F
	{2, CHIPTYPE_SN76496_SENTINEL, 0}, {2, DEVID_AY8910, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, // 0x30-0x37: 30=SN76489(2nd) 31=AY8910 stereo
	{2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, CHIPTYPE_SN76496_SENTINEL, 0}, // 0x38-0x3F: 3F=GG stereo(2nd)
	{3, 0, 0}, {3, 0, 0}, {3, 0, 0}, {3, 0, 0}, {3, 0, 0}, {3, 0, 0}, {3, 0, 0}, {3, 0, 0}, // 0x40-0x47: 40=Mikey
	{3, 0, 0}, {3, 0, 0}, {3, 0, 0}, {3, 0, 0}, {3, 0, 0}, {3, 0, 0}, {3, 0, 0}, {2, CHIPTYPE_SN76496_SENTINEL, 0}, // 0x48-0x4F: 4F=GG stereo
	{2, CHIPTYPE_SN76496_SENTINEL, 0}, {3, DEVID_YM2413, 0}, {3, DEVID_YM2612, 0}, {3, DEVID_YM2612, 0}, {3, DEVID_YM2151, 0}, {3, DEVID_YM2203, 0}, {3, DEVID_YM2608, 0}, {3, DEVID_YM2608, 0}, // 0x50-0x57: 50=SN76489 51=YM2413 52=YM2612p0 53=YM2612p1 54=YM2151 55=YM2203 56=YM2608p0 57=YM2608p1
	{3, DEVID_YM2610, 0}, {3, DEVID_YM2610, 0}, {3, DEVID_YM3812, 0}, {3, DEVID_YM3526, 0}, {3, DEVID_Y8950, 0}, {3, DEVID_YMZ280B, 0}, {3, DEVID_YMF262, 0}, {3, DEVID_YMF262, 0}, // 0x58-0x5F: 58=YM2610p0 59=YM2610p1 5A=YM3812 5B=YM3526 5C=Y8950 5D=YMZ280B 5E=YMF262p0 5F=YMF262p1
	{1, 0, 0}, {3, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0x60-0x67: 61=wait N samples(3 bytes, overridden in switch) 66=end 67=data block(special)
	{12, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0x68-0x6F: 68=PCM RAM write(12 bytes)
	{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0x70-0x77
	{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0x78-0x7F
	{1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, // 0x80-0x87
	{1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, {1, DEVID_YM2612, 0}, // 0x88-0x8F
	{3, 0, 0}, {2, 0, 0}, {2, 0, 0}, {4, 0, 0}, {2, 0, 0}, {5, 0, 1}, {1, 0, 0}, {1, 0, 0}, // 0x90-0x97
	{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0x98-0x9F
	{3, DEVID_AY8910, 0}, {3, DEVID_YM2413, 0}, {3, DEVID_YM2612, 0}, {3, DEVID_YM2612, 0}, {3, DEVID_YM2151, 0}, {3, DEVID_YM2203, 0}, {3, DEVID_YM2608, 0}, {3, DEVID_YM2608, 0}, // 0xA0-0xA7: A0=AY8910 A1=YM2413(2nd) A2=YM2612p0(2nd) A3=YM2612p1(2nd) A4=YM2151(2nd) A5=YM2203(2nd) A6=YM2608p0(2nd) A7=YM2608p1(2nd)
	{3, DEVID_YM2610, 0}, {3, DEVID_YM2610, 0}, {3, DEVID_YM3812, 0}, {3, DEVID_YM3526, 0}, {3, DEVID_Y8950, 0}, {3, DEVID_YMZ280B, 0}, {3, DEVID_YMF262, 0}, {3, DEVID_YMF262, 0}, // 0xA8-0xAF: A8=YM2610p0(2nd) A9=YM2610p1(2nd) AA=YM3812(2nd) AB=YM3526(2nd) AC=Y8950(2nd) AD=YMZ280B(2nd) AE=YMF262p0(2nd) AF=YMF262p1(2nd)
	{3, DEVID_RF5C68, 0}, {3, DEVID_RF5C68, 0}, {3, DEVID_32X_PWM, 0}, {3, DEVID_GB_DMG, 0}, {3, DEVID_NES_APU, 0}, {3, DEVID_YMW258, 0}, {3, DEVID_uPD7759, 0}, {3, DEVID_OKIM6258, 0}, // 0xB0-0xB7: B0=RF5C68 B1=RF5C164 B2=PWM B3=GB_DMG B4=NES_APU B5=YMW258 B6=uPD7759 B7=OKIM6258
	{3, DEVID_OKIM6295, 0}, {3, DEVID_C6280, 0}, {3, DEVID_K053260, 0}, {3, DEVID_POKEY, 0}, {3, DEVID_WSWAN, 0}, {3, DEVID_SAA1099, 0}, {3, DEVID_ES5506, 0}, {3, DEVID_GA20, 0}, // 0xB8-0xBF: B8=OKIM6295 B9=HuC6280 BA=K053260 BB=POKEY BC=WonderSwan BD=SAA1099 BE=ES5506 BF=GA20
	{5, DEVID_SEGAPCM, 1}, {5, DEVID_RF5C68, 1}, {5, DEVID_RF5C68, 1}, {4, DEVID_YMW258, 0}, {4, DEVID_ES5506, 0}, {4, DEVID_K054539, 0}, {4, DEVID_C140, 0}, {4, DEVID_C219, 0}, // 0xC0-0xC7
	{4, DEVID_K053260, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0xC8-0xCF
	{4, DEVID_YMF278B, 0}, {4, DEVID_YMF271, 0}, {4, DEVID_K051649, 0}, {5, DEVID_K054539, 0}, {5, DEVID_C140, 0}, {5, 0, 0}, {5, 0, 0}, {1, 0, 0}, // 0xD0-0xD7: D0=YMF278B D1=YMF271 D2=K051649 D3=K054539 D4=C140
	{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0xD8-0xDF
	{5, DEVID_YM2612, 0}, {5, DEVID_C352, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0xE0-0xE7
	{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0xE8-0xEF
	{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, // 0xF0-0xF7
	{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}  // 0xF8-0xFF
};

// Basic Gzip inflation helper
static std::vector<UINT8> InflateGzip(const UINT8* data, size_t size)
{
    std::vector<UINT8> out_data;
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = size;
    strm.next_in = (Bytef*)data;

    // Using 15+32 to enable gzip and zlib header detection automatically
    if (inflateInit2(&strm, 15 + 32) != Z_OK) {
        return out_data;
    }

    const size_t CHUNK = 16384;
    unsigned char out_chunk[CHUNK];
    int ret;

    do {
        strm.avail_out = CHUNK;
        strm.next_out = out_chunk;
        ret = inflate(&strm, Z_NO_FLUSH);
        switch (ret) {
        case Z_NEED_DICT:
        case Z_DATA_ERROR:
        case Z_MEM_ERROR:
            inflateEnd(&strm);
            return std::vector<UINT8>();
        }
        size_t have = CHUNK - strm.avail_out;
        out_data.insert(out_data.end(), out_chunk, out_chunk + have);
    } while (strm.avail_out == 0);

    inflateEnd(&strm);
    return out_data;
}


VGMFile::VGMFile() :
    _fileData(NULL),
    _fileType(0),
    _header(new VGM_HEADER()),
    _cpcUTF16(NULL)
{
    memset(_header, 0, sizeof(VGM_HEADER));
    CPConv_Init(&_cpcUTF16, "UTF-16LE", "UTF-8");
}

VGMFile::~VGMFile()
{
    Unload();
    if (_cpcUTF16)
    {
        CPConv_Deinit(_cpcUTF16);
    }
    delete _header;
}

UINT8 VGMFile::DetectFileType(DATA_LOADER* dLoad)
{
    const UINT8* data = DataLoader_GetData(dLoad);
    size_t size = DataLoader_GetSize(dLoad);

    if (size < 4) return 0;

    if (memcmp(data, "Vgm ", 4) == 0) return 1; // VGM
    if (size >= 2 && data[0] == 0x1F && data[1] == 0x8B) return 2; // VGZ
    if (memcmp(data, "S98", 3) == 0) return 3; // S98

    return 0; // Unknown
}

bool VGMFile::Load(DATA_LOADER* dLoad)
{
    Unload();

    if (!dLoad) {
        fprintf(stderr, "ERROR: VGMFile::Load received a NULL data loader.\n");
        return false;
    }

    DataLoader_ReadAll(dLoad);
    _fileType = DetectFileType(dLoad);

    switch (_fileType)
    {
    case 1: // VGM
        return ParseVGM(dLoad) == 0;
    case 2: // VGZ
        {
            const UINT8* compressed_data = DataLoader_GetData(dLoad);
            size_t compressed_size = DataLoader_GetSize(dLoad);
            _decompressedData = InflateGzip(compressed_data, compressed_size);
            if (_decompressedData.empty()) {
                fprintf(stderr, "ERROR: VGZ decompression failed.\n");
                return false;
            }
            // Create a new data loader for the decompressed data
            DATA_LOADER mem_dload;
            // This is still a bit of a hack, as we're not using a real memory loader,
            // but it's safer now that the data is owned by the class.
            DataLoader_Setup(&mem_dload, NULL, &_decompressedData[0]);
            _fileData = _decompressedData.data();
            // We need to manually set the size for the fake loader
            mem_dload._bytesLoaded = _decompressedData.size();
            mem_dload._bytesTotal = _decompressedData.size();

            return ParseVGM(&mem_dload) == 0;
        }
    case 3: // S98
        return ParseS98(dLoad) == 0;
    default:
        fprintf(stderr, "ERROR: Unknown or unsupported file type.\n");
        return false;
    }
}

UINT8 VGMFile::ParseVGM(DATA_LOADER* dLoad)
{
    _fileData = DataLoader_GetData(dLoad);
    if (DataLoader_GetSize(dLoad) < 0x40)
    {
        return 1;
    }

    if (ParseHeader(dLoad) != 0)
    {
        fprintf(stderr, "ERROR: VGMFile::Load: ParseHeader failed.\n");
        return 2;
    }
    
    LoadTags(dLoad);
    ParseVGMCommands(dLoad);

    return 0;
}

UINT8 VGMFile::ParseS98(DATA_LOADER* dLoad)
{
    (void)dLoad;
    // To be implemented
    fprintf(stderr, "S98 parsing is not yet implemented.\n");
    return 0;
}


void VGMFile::Unload()
{
    _fileData = NULL;
    _fileType = 0;
    _decompressedData.clear();
    _events.clear();
    memset(_header, 0, sizeof(VGM_HEADER));
    _tags = GD3_TAGS();
}

const VGM_HEADER* VGMFile::GetHeader() const
{
    return _header;
}

const GD3_TAGS& VGMFile::GetTags() const
{
    return _tags;
}

const std::vector<VgmEvent>& VGMFile::GetEvents() const
{
    return _events;
}

UINT8 VGMFile::ParseHeader(DATA_LOADER* dLoad)
{
    memset(_header, 0, sizeof(VGM_HEADER));

    _header->fileVer = ReadLE32(&_fileData[0x08]);
    _header->eofOfs = ReadRelOfs(_fileData, 0x04);
    _header->gd3Ofs = ReadRelOfs(_fileData, 0x14);
    _header->numTicks = ReadLE32(&_fileData[0x18]);
    _header->loopOfs = ReadRelOfs(_fileData, 0x1C);
    _header->loopTicks = ReadLE32(&_fileData[0x20]);
    _header->recordHz = ReadLE32(&_fileData[0x24]);

    if (_header->fileVer >= 0x150)
    {
        _header->dataOfs = ReadRelOfs(_fileData, 0x34);
        if (!_header->dataOfs) _header->dataOfs = 0x40; // spec: field=0 means default 0x40
    }
    else
        _header->dataOfs = 0x40;

    if (!_header->eofOfs || _header->eofOfs > DataLoader_GetSize(dLoad))
        _header->eofOfs = DataLoader_GetSize(dLoad);

    _header->dataEnd = _header->eofOfs;
    if (_header->gd3Ofs && (_header->gd3Ofs < _header->dataEnd && _header->gd3Ofs >= _header->dataOfs))
        _header->dataEnd = _header->gd3Ofs;

    return 0;
}

UINT8 VGMFile::LoadTags(DATA_LOADER* dLoad)
{
    (void)dLoad;
    if (!_header->gd3Ofs) return 0x01; // No tags
    if (_header->gd3Ofs >= _header->eofOfs) return 0x02; // Invalid offset

    if (_header->gd3Ofs + 0x0C > _header->eofOfs) return 0x03;
    if (memcmp(&_fileData[_header->gd3Ofs], "Gd3 ", 4) != 0) return 0x04;

    UINT32 tagVer = ReadLE32(&_fileData[_header->gd3Ofs + 0x04]);
    if (tagVer < 0x100) return 0x05; // Unsupported version

    UINT32 eotPos = ReadLE32(&_fileData[_header->gd3Ofs + 0x08]);
    UINT32 curPos = _header->gd3Ofs + 0x0C;
    eotPos += curPos;
    if (eotPos > _header->eofOfs) eotPos = _header->eofOfs;

    std::vector<std::string> tagStrings;
    while (curPos < eotPos)
    {
        UINT32 startPos = curPos;
        while (curPos < eotPos && ReadLE16(&_fileData[curPos]) != L'\0')
            curPos += 2;
        tagStrings.push_back(GetUTF8String(&_fileData[startPos], &_fileData[curPos]));
        curPos += 2; // Skip null terminator
    }

    if (tagStrings.size() > 0) _tags.track_name_en = tagStrings[0];
    if (tagStrings.size() > 1) _tags.track_name_jp = tagStrings[1];
    if (tagStrings.size() > 2) _tags.game_name_en = tagStrings[2];
    if (tagStrings.size() > 3) _tags.game_name_jp = tagStrings[3];
    if (tagStrings.size() > 4) _tags.system_name_en = tagStrings[4];
    if (tagStrings.size() > 5) _tags.system_name_jp = tagStrings[5];
    if (tagStrings.size() > 6) _tags.artist_en = tagStrings[6];
    if (tagStrings.size() > 7) _tags.artist_jp = tagStrings[7];
    if (tagStrings.size() > 8) _tags.release_date = tagStrings[8];
    if (tagStrings.size() > 9) _tags.vgm_creator = tagStrings[9];
    if (tagStrings.size() > 10) _tags.notes = tagStrings[10];

    return 0;
}

std::string VGMFile::GetUTF8String(const UINT8* startPtr, const UINT8* endPtr)
{
    if (!_cpcUTF16 || startPtr >= endPtr) return "";

    size_t convSize = 0;
    char* convData = NULL;
    CPConv_StrConvert(_cpcUTF16, &convSize, &convData, endPtr - startPtr, (const char*)startPtr);
    if (!convData) return "";

    std::string result(convData, convSize);
    free(convData);
    return result;
}

void VGMFile::ParseVGMCommands(DATA_LOADER* dLoad)
{
    UINT32 filePos = _header->dataOfs;
    UINT32 tick = 0;

    _events.clear();
    _parseDbg = std::string();
    char _dbgbuf[256];
    snprintf(_dbgbuf, sizeof(_dbgbuf), "[Parser] dataOfs=0x%X dataEnd=0x%X GetSize=%u fileData=%p\n",
        filePos, _header->dataEnd, DataLoader_GetSize(dLoad), (void*)_fileData);
    _parseDbg += _dbgbuf;
    if (filePos == 0 || filePos >= DataLoader_GetSize(dLoad)) {
        snprintf(_dbgbuf, sizeof(_dbgbuf), "[Parser] EARLY RETURN: filePos=0x%X GetSize=%u\n", filePos, DataLoader_GetSize(dLoad));
        _parseDbg += _dbgbuf;
        return;
    }

    while (filePos < _header->dataEnd)
    {
        VgmEvent event;
        memset(&event, 0, sizeof(event));
        event.tick = tick;
        UINT8 cmd = _fileData[filePos];
        event.cmd = cmd;

        const VGM_COMMAND_INFO& cmdInfo = VGM_CMD_INFO[cmd];
        UINT32 cmdLen = cmdInfo.cmdLen;

        bool isRegWrite = false;

        if (cmdInfo.chipType != 0) {
            isRegWrite = true;
            // Map sentinel back to real DEVID (DEVID_SN76496==0 can't be used directly as non-zero guard)
            event.chip_type = (cmdInfo.chipType == CHIPTYPE_SN76496_SENTINEL) ? DEVID_SN76496 : cmdInfo.chipType;
            // 0xA1-0xAF = second chip versions of 0x51-0x5F => chip_num=1
            // 0xB0-0xBF = extended chips (SAA1099, GA20, etc.) => chip_num=0 (first instance)
            // 0x30-0x3F = second chip versions of some 1-byte/2-byte commands => chip_num=1
            if ((cmd >= 0xA1 && cmd <= 0xAF) || (cmd >= 0x30 && cmd <= 0x3F))
                event.chip_num = 1;
            else
                event.chip_num = 0;
            // 0x80-0x8F: YM2612 DAC write + wait N samples (N = cmd & 0x0F)
            if (cmd >= 0x80 && cmd <= 0x8F)
                tick += (cmd & 0x0F);
            // For 4-byte commands (0xD0-0xDF), chip_num is in port bit 7
            switch(cmdLen) {
                case 2: // e.g. 0x50
                    event.data = _fileData[filePos + 1];
                    break;
                case 3: // e.g. 0x51-0x5F, 0xA1-0xAF
                    event.addr = _fileData[filePos + 1];
                    event.data = _fileData[filePos + 2];
                    // Port assignment per VGM spec (same for chip 0 and chip 1):
                    if (cmd == 0x52 || cmd == 0xA2) event.port = 0;  // YM2612 port0
                    else if (cmd == 0x53 || cmd == 0xA3) event.port = 1;  // YM2612 port1
                    else if (cmd == 0x56 || cmd == 0xA6) event.port = 0;  // YM2608 port0
                    else if (cmd == 0x57 || cmd == 0xA7) event.port = 1;  // YM2608 port1
                    else if (cmd == 0x58 || cmd == 0xA8) event.port = 0;  // YM2610 port0
                    else if (cmd == 0x59 || cmd == 0xA9) event.port = 1;  // YM2610 port1
                    else if (cmd == 0x5E || cmd == 0xAE) event.port = 0;  // YMF262 port0
                    else if (cmd == 0x5F || cmd == 0xAF) event.port = 1;  // YMF262 port1
                    else if (cmd == 0xB2) { // 32X PWM: addr=channel(0-4), data16=12-bit value
                        event.addr = (_fileData[filePos + 1] >> 4) & 0x0F;
                        event.data16 = (((UINT16)_fileData[filePos + 1] << 8) | _fileData[filePos + 2]) & 0x0FFF;
                    }
                    break;
                case 4: // e.g. 0xD0-0xD6: port byte has chip_num in bit7
                    event.chip_num = (_fileData[filePos + 1] >> 7) & 1;
                    event.port = _fileData[filePos + 1] & 0x7F;
                    event.addr = _fileData[filePos + 2];
                    event.data = _fileData[filePos + 3];
                    break;
                case 5: // e.g. 0xE1 C352: chip_num in bit15 of addr, 16-bit addr (BE), 16-bit data (BE)
                    event.chip_num = (_fileData[filePos + 1] >> 7) & 1;
                    event.port = _fileData[filePos + 1] & 0x7F; // addr high byte
                    event.addr = _fileData[filePos + 2];          // addr low byte
                    event.data16 = ((UINT16)_fileData[filePos + 3] << 8) | _fileData[filePos + 4];
                    event.data = _fileData[filePos + 3];          // data high byte
                    break;
            }
        } else {
            switch (cmd)
            {
                case 0x61:
                    tick += ReadLE16(&_fileData[filePos + 1]);
                    break;
                case 0x62:
                    tick += 735;
                    break;
                case 0x63:
                    tick += 882;
                    break;
                case 0x66: // End of data
                    filePos = _header->dataEnd;
                    break;
                case 0x67: // Data block
                    cmdLen = 7 + ReadLE32(&_fileData[filePos + 3]);
                    break;
                case 0xE0: // PCM seek
                    cmdLen = 5;
                    break;
                default:
                    if (cmd >= 0x70 && cmd <= 0x7F) { // wait n+1 samples
                        tick += (cmd & 0x0F) + 1;
                        cmdLen = 1;
                    } else if (cmd >= 0x80 && cmd <= 0x8F) { // YM2612 pcm write + wait
                        isRegWrite = true;
                        event.chip_type = DEVID_YM2612;
                        // This is a special case, not a standard register write.
                        // We'll just pass the command itself for now.
                        tick += (cmd & 0x0F);
                        cmdLen = 1;
                    } else if (cmdLen == 0) {
                        // This should not happen for valid commands
                        fprintf(stderr, "Unknown VGM command: 0x%02X at offset 0x%X\n", cmd, filePos);
                        filePos = _header->dataEnd; // Stop parsing
                    }
                    break;
            }
        }

        if (isRegWrite) {
            _events.push_back(event);
        }

        if (filePos >= _header->dataEnd) break;
        filePos += cmdLen;
    }
}

void VGMFile::ParseS98Commands(DATA_LOADER* dLoad)
{
    (void)dLoad;
    // To be implemented
}
