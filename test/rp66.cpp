#include <vector>
#include <cstring>

#include <catch2/catch.hpp>

#include <lfp/memfile.h>
#include <lfp/rp66.h>
#include <lfp/lfp.h>

#include "utils.hpp"

using namespace Catch::Matchers;

struct random_rp66 : random_memfile {
    random_rp66() : mem(copy()) {
        REQUIRE(not expected.empty());
    }

    ~random_rp66() {
        lfp_close(mem);
    }

    /*
     * The rp66 creation must still be parameterised on number-of-records
     * and size-of-records, which in catch cannot be passed to the constructor
     *
     * Only fixed-size records are made, which is slightly unfortunate.
     * However, generating consistent, variable-length records are a lot more
     * subtle and complicated, and it's reasonable to assume that a lot of rp66
     * files use fixed-size records anyway.
     *
     * TODO: variable-length records
     */
    void make(int records) {
        REQUIRE(records > 0);
        // constants defined by the format
        const char format = 0xFF;
        const std::uint8_t major = 1;

        const std::int64_t record_size = std::ceil(double(size) / records);
        assert(record_size + 4 <= std::numeric_limits< std::uint16_t >::max());

        INFO("Partitioning " << size << " bytes into "
            << records << " records of max "
            << record_size << " bytes"
        );

        auto src = std::begin(expected);
        std::int64_t remaining = expected.size();
        REQUIRE(remaining > 0);
        for (int i = 0; i < records; ++i) {
            const uint16_t n = std::min(record_size, remaining);
            auto head = std::vector< unsigned char >(4, 0);
            const uint16_t m = n + 4;
            std::memcpy(head.data() + 0, &m,  sizeof(m));
            std::memcpy(head.data() + 2, &format, sizeof(format));
            std::memcpy(head.data() + 3, &major,  sizeof(major));

            bytes.insert(bytes.end(), head.begin(), head.end());
            bytes.insert(bytes.end(), src, src + n);
            src += n;
            remaining -= n;
        }
        REQUIRE(remaining == 0);
        REQUIRE(src == std::end(expected));

        REQUIRE(bytes.size() == expected.size() + records * 4);
        lfp_close(f);
        f = nullptr;
        auto* tmp = lfp_memfile_openwith(bytes.data(), bytes.size());
        REQUIRE(tmp);
        f = lfp_rp66_open(tmp);
        REQUIRE(f);
    }

    std::vector< unsigned char > bytes;
    lfp_protocol* mem = nullptr;
};

TEST_CASE(
    "Empty file can be opened, reads zero bytes",
    "[visible envelope][rp66][empty]") {
    const auto file = std::vector< unsigned char > {
        /* First VE */
        0x04, 0x00,
        0xFF, 0x01,
        /* Second VE */
        0x04, 0x00,
        0xFF, 0x01,
        /* Third VE */
        0x04, 0x00,
        0xFF, 0x01,
    };

    auto* mem  = lfp_memfile_openwith(file.data(), file.size());
    auto* rp66 = lfp_rp66_open(mem);

    auto out = std::vector< unsigned char >(5, 0xFF);
    std::int64_t bytes_read = -1;
    const auto err = lfp_readinto(rp66, out.data(), 5, &bytes_read);

    CHECK(bytes_read == 0);
    CHECK(err == LFP_OKINCOMPLETE);
    lfp_close(rp66);
}

TEST_CASE(
    "Reads 8 bytes from 8-bytes file",
    "[visible envelope][rp66]") {
    const auto file = std::vector< unsigned char > {
        /* First VE */
        0x0C, 0x00,
        0xFF, 0x01,

        /* Body */
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    };
    const auto expected = std::vector< unsigned char > {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    };

    auto* mem = lfp_memfile_openwith(file.data(), file.size());
    auto* rp66 = lfp_rp66_open(mem);

    auto out = std::vector< unsigned char >(8, 0xFF);
    std::int64_t bytes_read = -1;
    const auto err = lfp_readinto(rp66, out.data(), 8, &bytes_read);

    CHECK(bytes_read == 8);
    CHECK(err == LFP_OK);
    CHECK_THAT(out, Equals(expected));
    lfp_close(rp66);
}

TEST_CASE(
    "Read passed end-of-file"
    "[visible envelope][rp66]") {

    const auto file = std::vector< unsigned char > {
        /* First Visible Envelope */
        0x0C, 0x00,
        0xFF, 0x01,
        /* Body */
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        /* Second Visible Envelope */
        0x06, 0x00,
        0xFF, 0x01,
        /* Body */
        0x09, 0x0A,
    };
    const auto expected = std::vector< unsigned char > {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
    };

    auto* mem = lfp_memfile_openwith(file.data(), file.size());
    auto* rp66 = lfp_rp66_open(mem);

    auto out = std::vector< unsigned char >(10, 0xFF);
    std::int64_t bytes_read = -1;
    const auto err = lfp_readinto(rp66, out.data(), 12, &bytes_read);

    CHECK(bytes_read == 10);
    CHECK(err == LFP_OKINCOMPLETE);
    CHECK_THAT(out, Equals(expected));

    lfp_close(rp66);
}

TEST_CASE_METHOD(
    random_rp66,
    "Visible Envelope: A file can be read in a single read",
    "[visible envelope][rp66]") {
    const auto records = GENERATE(1, 2, 3, 5, 8, 13);
    make(records);

    std::int64_t nread = 0;
    const auto err = lfp_readinto(f, out.data(), out.size(), &nread);

    CHECK(err == LFP_OK);
    CHECK(nread == expected.size());
    CHECK_THAT(out, Equals(expected));
}

TEST_CASE_METHOD(
    random_rp66,
    "Visible Envelope: A file can be read in multiple, smaller reads",
    "[visible envelope][rp66]") {
    const auto records = GENERATE(1, 2, 3, 5, 8, 13);
    make(records);

    // +1 so that if size is 1, max is still >= min
    const auto readsize = GENERATE_COPY(take(1, random(1, (size + 1) / 2)));
    const auto complete_reads = size / readsize;

    auto* p = out.data();
    std::int64_t nread = 0;
    for (int i = 0; i < complete_reads; ++i) {
        const auto err = lfp_readinto(f, p, readsize, &nread);
        CHECK(err == LFP_OK);
        CHECK(nread == readsize);
        p += nread;
    }

    if (size % readsize != 0) {
        const auto err = lfp_readinto(f, p, readsize, &nread);
        CHECK(err == LFP_OKINCOMPLETE);
    }

    CHECK_THAT(out, Equals(expected));
}

TEST_CASE_METHOD(
    random_rp66,
    "Visible Envelope: single seek matches underlying handle",
    "[visible envelope][rp66]") {
    const auto real_size = size;
    auto records = GENERATE(1, 2, 3, 5, 8, 13);
    make(records);

    const auto n = GENERATE_COPY(take(1, random(0, real_size - 1)));
    auto err = lfp_seek(f, n);
    CHECK(err == LFP_OK);

    const auto remaining = real_size - n;
    out.resize(remaining);
    auto memout = out;

    std::int64_t nread = 0;
    err = lfp_readinto(f, out.data(), out.size(), &nread);
    CHECK(err == LFP_OK);

    auto memerr = lfp_seek(mem, n);
    CHECK(memerr == LFP_OK);
    std::int64_t memnread = 0;
    memerr = lfp_readinto(mem, memout.data(), memout.size(), &memnread);
    CHECK(memerr == LFP_OK);

    CHECK(nread == memnread);
    CHECK(nread == out.size());
    CHECK_THAT(out, Equals(memout));
}

TEST_CASE_METHOD(
    random_rp66,
    "Visible Envelope: multiple seeks and tells match underlying handle",
    "[visible envelope][rp66]") {
    const auto real_size = size;
    const auto records = GENERATE(1, 2, 3, 5, 8, 13);
    make(records);

    for (int i = 0; i < 4; ++i) {
        const auto n = GENERATE_COPY(take(1, random(0, real_size - 1)));
        auto err = lfp_seek(f, n);
        CHECK(err == LFP_OK);

        auto memerr = lfp_seek(mem, n);
        CHECK(memerr == LFP_OK);

        std::int64_t tape_tell;
        std::int64_t mem_tell;
        err = lfp_tell(f, &tape_tell);
        CHECK(err == LFP_OK);
        memerr = lfp_tell(mem, &mem_tell);
        CHECK(memerr == LFP_OK);
        CHECK(tape_tell == mem_tell);
    }

    const auto n = GENERATE_COPY(take(1, random(0, real_size - 1)));
    const auto remaining = real_size - n;
    out.resize(remaining);
    auto memout = out;

    auto err = lfp_seek(f, n);
    CHECK(err == LFP_OK);
    std::int64_t nread = 0;
    err = lfp_readinto(f, out.data(), out.size(), &nread);
    CHECK(err == LFP_OK);

    auto memerr = lfp_seek(mem, n);
    CHECK(memerr == LFP_OK);
    std::int64_t memnread = 0;
    memerr = lfp_readinto(mem, memout.data(), memout.size(), &memnread);
    CHECK(memerr == LFP_OK);

    CHECK(nread == memnread);
    CHECK(nread == out.size());
    CHECK_THAT(out, Equals(memout));
}
