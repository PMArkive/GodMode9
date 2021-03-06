#include "ticket.h"
#include "unittype.h"
#include "cert.h"
#include "sha.h"
#include "rsa.h"
#include "ff.h"

u32 ValidateTicket(Ticket* ticket) {
    static const u8 magic[] = { TICKET_SIG_TYPE };
    if ((memcmp(ticket->sig_type, magic, sizeof(magic)) != 0) ||
        ((strncmp((char*) ticket->issuer, TICKET_ISSUER, 0x40) != 0) &&
        (strncmp((char*) ticket->issuer, TICKET_ISSUER_DEV, 0x40) != 0)) ||
        (ticket->commonkey_idx >= 6) ||
        (getbe32(&ticket->content_index[0]) != 0x10014) ||
        (getbe32(&ticket->content_index[4]) < 0x14) ||
        (getbe32(&ticket->content_index[12]) != 0x10014) ||
        (getbe32(&ticket->content_index[16]) != 0))
        return 1;
    return 0;
}

u32 ValidateTwlTicket(Ticket* ticket) {
    static const u8 magic[] = { TICKET_SIG_TYPE_TWL };
    if ((memcmp(ticket->sig_type, magic, sizeof(magic)) != 0) ||
        (strncmp((char*) ticket->issuer, TICKET_ISSUER_TWL, 0x40) != 0))
        return 1;
    return 0;
}

u32 ValidateTicketSignature(Ticket* ticket) {
    Certificate cert;

    // grab cert from certs.db
    if (LoadCertFromCertDb(&cert, (char*)(ticket->issuer)) != 0)
        return 1;

    int ret = Certificate_VerifySignatureBlock(&cert, &(ticket->signature), 0x100, (void*)&(ticket->issuer), GetTicketSize(ticket) - 0x140, true);

    Certificate_Cleanup(&cert);

    return ret;
}

u32 BuildFakeTicket(Ticket* ticket, u8* title_id) {
    static const u8 sig_type[4] =  { TICKET_SIG_TYPE }; // RSA_2048 SHA256
    static const u8 ticket_cnt_index[] = { // whatever this is
        0x00, 0x01, 0x00, 0x14, 0x00, 0x00, 0x00, 0xAC, 0x00, 0x00, 0x00, 0x14, 0x00, 0x01, 0x00, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x84,
        0x00, 0x00, 0x00, 0x84, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    // set ticket all zero for a clean start
    memset(ticket, 0x00, TICKET_COMMON_SIZE); // 0xAC being size of this fake ticket's content index
    // fill ticket values
    memcpy(ticket->sig_type, sig_type, 4);
    memset(ticket->signature, 0xFF, 0x100);
    snprintf((char*) ticket->issuer, 0x40, IS_DEVKIT ? TICKET_ISSUER_DEV : TICKET_ISSUER);
    memset(ticket->ecdsa, 0xFF, 0x3C);
    ticket->version = 0x01;
    memset(ticket->titlekey, 0xFF, 16);
    memcpy(ticket->title_id, title_id, 8);
    ticket->commonkey_idx = 0x00; // eshop
    ticket->audit = 0x01; // whatever
    memcpy(ticket->content_index, ticket_cnt_index, sizeof(ticket_cnt_index));
    memset(&ticket->content_index[sizeof(ticket_cnt_index)], 0xFF, 0x80); // 1024 content indexes

    return 0;
}

u32 GetTicketContentIndexSize(const Ticket* ticket) {
    return getbe32(&ticket->content_index[4]);
}

u32 GetTicketSize(const Ticket* ticket) {
    return sizeof(Ticket) + GetTicketContentIndexSize(ticket);
}

u32 BuildTicketCert(u8* tickcert) {
    static const u8 cert_hash_expected[0x20] = {
        0xDC, 0x15, 0x3C, 0x2B, 0x8A, 0x0A, 0xC8, 0x74, 0xA9, 0xDC, 0x78, 0x61, 0x0E, 0x6A, 0x8F, 0xE3,
        0xE6, 0xB1, 0x34, 0xD5, 0x52, 0x88, 0x73, 0xC9, 0x61, 0xFB, 0xC7, 0x95, 0xCB, 0x47, 0xE6, 0x97
    };
    static const u8 cert_hash_expected_dev[0x20] = {
        0x97, 0x2A, 0x32, 0xFF, 0x9D, 0x4B, 0xAA, 0x2F, 0x1A, 0x24, 0xCF, 0x21, 0x13, 0x87, 0xF5, 0x38,
        0xC6, 0x4B, 0xD4, 0x8F, 0xDF, 0x13, 0x21, 0x3D, 0xFC, 0x72, 0xFC, 0x8D, 0x9F, 0xDD, 0x01, 0x0E
    };

    static const char* const retail_issuers[] = {"Root-CA00000003-XS0000000c", "Root-CA00000003"};
    static const char* const dev_issuers[] = {"Root-CA00000004-XS00000009", "Root-CA00000004"};

    size_t size = TICKET_CDNCERT_SIZE;
    if (BuildRawCertBundleFromCertDb(tickcert, &size, !IS_DEVKIT ? retail_issuers : dev_issuers, 2) ||
        size != TICKET_CDNCERT_SIZE) {
        return 1;
    }

    // check the certificate hash
    u8 cert_hash[0x20];
    sha_quick(cert_hash, tickcert, TICKET_CDNCERT_SIZE, SHA256_MODE);
    if (memcmp(cert_hash, IS_DEVKIT ? cert_hash_expected_dev : cert_hash_expected, 0x20) != 0)
        return 1;

    return 0;
}

u32 TicketRightsCheck_InitContext(TicketRightsCheck* ctx, Ticket* ticket) {
    if (!ticket || ValidateTicket(ticket)) return 1;

    const TicketContentIndexMainHeader* mheader = (const TicketContentIndexMainHeader*)&ticket->content_index[0];
    u32 dheader_pos = getbe32(&mheader->data_header_relative_offset[0]);
    u32 cindex_size = getbe32(&mheader->content_index_size[0]);

    // data header is not inbounds, so it's not valid for use
    if (cindex_size < dheader_pos || dheader_pos + sizeof(TicketContentIndexDataHeader) > cindex_size) return 1;

    const TicketContentIndexDataHeader* dheader = (const TicketContentIndexDataHeader*)&ticket->content_index[dheader_pos];
    u32 data_pos = getbe32(&dheader->data_relative_offset[0]);
    u32 count = getbe32(&dheader->max_entry_count[0]);
    u32 data_max_size = cindex_size - data_pos;

    count = min(data_max_size / sizeof(TicketRightsField), count);

    // if no entries or data type isn't what we want or not enough space for at least one entry,
    // it still is valid, but it will just follow other rules
    if (count == 0 || getbe16(&dheader->data_type[0]) != 3) {
        ctx->count = 0;
        ctx->rights = NULL;
    } else {
        ctx->count = count;
        ctx->rights = (const TicketRightsField*)&ticket->content_index[data_pos];
    }

    return 0;
}

bool TicketRightsCheck_CheckIndex(TicketRightsCheck* ctx, u16 index) {
    if (ctx->count == 0) return index < 256; // when no fields, true if below 256

    bool hasright = false;

    // it loops until one of these happens:
    // - we run out of bit fields
    // - at the first encounter of an index offset field that's bigger than index
    // - at the first encounter of a positive indicator of content rights
    for (u32 i = 0; i < ctx->count; i++) {
        u16 indexoffset = getbe16(&ctx->rights[i].indexoffset[0]);
        if (index < indexoffset) break;
        u16 bitpos = index - indexoffset;
        if (bitpos >= 1024) continue; // not in this field
        if (ctx->rights[i].rightsbitfield[bitpos / 8] & (1 << (bitpos % 8))) {
            hasright = true;
            break;
        }
    }

    return hasright;
}
