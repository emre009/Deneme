#include "firm.h"
#include "aes.h"
#include "sha.h"
#include "nand.h"
#include "keydb.h"
#include "unittype.h"
#include "ff.h"

// 0 -> pre 9.5 / 1 -> 9.5 / 2 -> post 9.5
#define A9L_CRYPTO_TYPE(hdr) ((hdr->k9l[3] == 0xFF) ? 0 : (hdr->k9l[3] == '1') ? 1 : 2)

// valid addresses for FIRM section loading, pairs of start / end address, provided by Wolfvak
#define FIRM_VALID_ADDRESS  \
    0x08006000, 0x08800000, \
    0x18000000, 0x18600000, \
    0x1FFF0000, 0x1FFFFFFC, \
    0x20000000, 0x23FFFE00, \
    0x24000000, 0x27FFFB00

u32 ValidateFirmHeader(FirmHeader* header, u32 data_size) {
    u8 magic[] = { FIRM_MAGIC };
    if (memcmp(header->magic, magic, sizeof(magic)) != 0)
        return 1;
    
    u32 firm_size = sizeof(FirmHeader);
    int section_arm11 = -1;
    int section_arm9 = -1;
    for (u32 i = 0; i < 4; i++) {
        FirmSectionHeader* section = header->sections + i;
        if (!section->size) continue;
        if (section->offset < firm_size) return 1;
        if ((header->entry_arm11 >= section->address) &&
            (header->entry_arm11 < section->address + section->size))
            section_arm11 = i;
        if ((header->entry_arm9 >= section->address) &&
            (header->entry_arm9 < section->address + section->size))
            section_arm9 = i;
        firm_size = section->offset + section->size;
    }
    
    if ((firm_size > FIRM_MAX_SIZE) || (data_size && (firm_size > data_size)))
        return 1;
    if ((header->entry_arm11 && (section_arm11 < 0)) || (header->entry_arm9 && (section_arm9 < 0)))
        return 1;
    
    return 0;
}

u32 ValidateFirmA9LHeader(FirmA9LHeader* header) {
    const u8 enckeyX0x15hash[0x20] = {
        0x0A, 0x85, 0x20, 0x14, 0x8F, 0x7E, 0xB7, 0x21, 0xBF, 0xC6, 0xC8, 0x82, 0xDF, 0x37, 0x06, 0x3C,
        0x0E, 0x05, 0x1D, 0x1E, 0xF3, 0x41, 0xE9, 0x80, 0x1E, 0xC9, 0x97, 0x82, 0xA0, 0x84, 0x43, 0x08
    };
    const u8 enckeyX0x15devhash[0x20] = {
        0xFC, 0x46, 0x74, 0x78, 0x73, 0x01, 0xD3, 0x23, 0x52, 0x94, 0x97, 0xED, 0xA8, 0x5B, 0xCF, 0xD2,
        0xDA, 0x2D, 0xFA, 0x47, 0x8E, 0x2D, 0x98, 0x89, 0xBA, 0x60, 0xE8, 0x43, 0x5C, 0x1B, 0x93, 0x65, 
    };
    return sha_cmp((IS_DEVKIT) ? enckeyX0x15devhash : enckeyX0x15hash, header->keyX0x15, 0x10, SHA256_MODE);
}

u32 ValidateFirm(void* firm, u32 firm_size) {
    FirmHeader* header = (FirmHeader*) firm;
    
    // validate firm header
    if (ValidateFirmHeader(header, firm_size) != 0)
        return 1;
    
    // hash verify all available sections and check load address
    for (u32 i = 0; i < 4; i++) {
        u32 valid_address[] = { FIRM_VALID_ADDRESS };
        FirmSectionHeader* section = header->sections + i;
        if (!section->size) continue;
        if (sha_cmp(section->hash, (u8*) firm + section->offset, section->size, SHA256_MODE) != 0) {
            return 1;
        }
        bool is_valid_address = false;
        for (u32 a = 0; a < sizeof(valid_address) / (2*sizeof(u32)); a++) {
            if ((valid_address[2*a] >= section->address) && (valid_address[(2*a)+1] <= section->address + section->size))
                is_valid_address = true;
        }
        if (!is_valid_address) return 1;
    }
    
    // section for arm9 entrypoint not found?
    if (!FindFirmArm9Section(header))
        return 1;
    
    return 0;
}

FirmSectionHeader* FindFirmArm9Section(FirmHeader* firm) {
    for (u32 i = 0; i < 4; i++) {
        FirmSectionHeader* section = firm->sections + i;
        if (section->size && (section->type == 0))
            return section;
    }
    return NULL;
}

u32 GetArm9BinarySize(FirmA9LHeader* a9l) {
    char* size_ascii = a9l->size_ascii;
    u32 size = 0;
    for (u32 i = 0; (i < 8) && *(size_ascii + i); i++)
        size = (size * 10) + (*(size_ascii + i) - '0');
    return size;
}

u32 SetupSecretKey(u32 keynum) {
    static u8 __attribute__((aligned(32))) sector[0x200];
    
    // safety check
    if (keynum >= 0x200/0x10) return 1;
    
    // seach for secret sector data...
    if (GetLegitSector0x96(sector) == 0) {
        setup_aeskey(0x11, sector + (keynum*0x10));
        use_aeskey(0x11);
        return 0;
    }
    
    // try to load from key database
    if ((keynum < 2) && (LoadKeyFromFile(NULL, 0x11, 'N', (keynum == 0) ? "95" : "96")))
        return 0; // key found in keydb, done
    
    // out of options
    return 1;
}

u32 DecryptA9LHeader(FirmA9LHeader* header) {
    u32 type = A9L_CRYPTO_TYPE(header);
    
    if (SetupSecretKey(0) != 0) return 1;
    aes_decrypt(header->keyX0x15, header->keyX0x15, 1, AES_CNT_ECB_DECRYPT_MODE);
    if (type) {
        if (SetupSecretKey((type == 1) ? 0 : 1) != 0) return 1;
        aes_decrypt(header->keyX0x16, header->keyX0x16, 1, AES_CNT_ECB_DECRYPT_MODE);
    }
    
    return 0;
}

u32 SetupArm9BinaryCrypto(FirmA9LHeader* header) {
    u32 type = A9L_CRYPTO_TYPE(header);
    
    if (type == 0) {
        u8 __attribute__((aligned(32))) keyX0x15[0x10];
        memcpy(keyX0x15, header->keyX0x15, 0x10);
        if (SetupSecretKey(0) != 0) return 1;
        aes_decrypt(keyX0x15, keyX0x15, 1, AES_CNT_ECB_DECRYPT_MODE);
        setup_aeskeyX(0x15, keyX0x15);
        setup_aeskeyY(0x15, header->keyY0x150x16);
        use_aeskey(0x15);
    } else {
        u8 __attribute__((aligned(32))) keyX0x16[0x10];
        memcpy(keyX0x16, header->keyX0x16, 0x10);
        if (SetupSecretKey((type == 1) ? 0 : 1) != 0) return 1;
        aes_decrypt(keyX0x16, keyX0x16, 1, AES_CNT_ECB_DECRYPT_MODE);
        setup_aeskeyX(0x16, keyX0x16);
        setup_aeskeyY(0x16, header->keyY0x150x16);
        use_aeskey(0x16);
    }
    
    return 0;
}

u32 DecryptArm9Binary(void* data, u32 offset, u32 size, FirmA9LHeader* a9l) {
    // offset == offset inside ARM9 binary
    // ARM9 binary begins 0x800 byte after the ARM9 loader header
    
    // only process actual ARM9 binary
    u32 size_bin = GetArm9BinarySize(a9l);
    if (offset >= size_bin) return 0;
    else if (size >= size_bin - offset)
        size = size_bin - offset;
    
    // decrypt data
    if (SetupArm9BinaryCrypto(a9l) != 0) return 1;
    ctr_decrypt_byte(data, data, size, offset, AES_CNT_CTRNAND_MODE, a9l->ctr);
    
    return 0;
}

u32 DecryptFirm(void* data, u32 offset, u32 size, FirmHeader* firm, FirmA9LHeader* a9l) {
    // ARM9 binary size / offset
    FirmSectionHeader* arm9s = FindFirmArm9Section(firm);
    u32 offset_arm9bin = arm9s->offset + ARM9BIN_OFFSET;
    u32 size_arm9bin = GetArm9BinarySize(a9l);
    
    // sanity checks
    if (!size_arm9bin || (size_arm9bin + ARM9BIN_OFFSET > arm9s->size))
        return 1; // bad header / data
    
    // check if ARM9 binary in data
    if ((offset_arm9bin >= offset + size) ||
        (offset >= offset_arm9bin + size_arm9bin))
        return 0; // section not in data
    
    // determine data / offset / size
    u8* data8 = (u8*)data;
    u8* data_i = data8;
    u32 offset_i = 0;
    u32 size_i = size_arm9bin;
    if (offset_arm9bin < offset)
        offset_i = offset - offset_arm9bin;
    else data_i = data8 + (offset_arm9bin - offset);
    size_i = size_arm9bin - offset_i;
    if (size_i > size - (data_i - data8))
        size_i = size - (data_i - data8);
    
    return DecryptArm9Binary(data_i, offset_i, size_i, a9l);
}

u32 DecryptFirmSequential(void* data, u32 offset, u32 size) {
    // warning: this will only work for sequential processing
    // unexpected results otherwise
    static FirmHeader firm = { 0 };
    static FirmA9LHeader a9l = { 0 };
    static FirmHeader* firmptr = NULL;
    static FirmA9LHeader* a9lptr = NULL;
    static FirmSectionHeader* arm9s = NULL;
    
    // fetch firm header from data
    if ((offset == 0) && (size >= sizeof(FirmHeader))) {
        memcpy(&firm, data, sizeof(FirmHeader));
        firmptr = (ValidateFirmHeader(&firm, 0) == 0) ? &firm : NULL;
        arm9s = (firmptr) ? FindFirmArm9Section(firmptr) : NULL;
        a9lptr = NULL;
    }
    
    // safety check, firm header pointer
    if (!firmptr) return 1;
    
    // fetch ARM9 loader header from data
    if (arm9s && !a9lptr && (offset <= arm9s->offset) &&
        ((offset + size) >= arm9s->offset + sizeof(FirmA9LHeader))) {
        memcpy(&a9l, (u8*)data + arm9s->offset - offset, sizeof(FirmA9LHeader));
        a9lptr = (ValidateFirmA9LHeader(&a9l) == 0) ? &a9l : NULL;
    }
    
    return (a9lptr) ? DecryptFirm(data, offset, size, firmptr, a9lptr) : 0;
}
