#include <cassert>
#include <limits>
#include <vector>

#include <fmt/format.h>

#include <lfp/protocol.hpp>
#include <lfp/rp66.h>

namespace lfp { namespace {

class rp66 : public lfp_protocol {
public:
    rp66(lfp_protocol*);

    // TODO: there must be a "reset" semantic for when there's a read error to
    // put it back into a valid state

    void close() noexcept (false) override;
    lfp_status readinto(void* dst, std::int64_t len, std::int64_t* bytes_read)
        noexcept (false) override;

    int eof() const noexcept (true);
    std::int64_t tell() const noexcept (true) override;
    void seek(std::int64_t) noexcept (false) override;

private:
    struct header {
        std::uint16_t  length;
        char           format;
        std::uint8_t   major;

        /*
         * Visible Envelopes only contain information relative to the start
         * of the current Visible Record. I.e. it does not contain any
         * information about what happened prior to this record.
         *
         * That makes the mapping between offsets of the underlying bytes and the
         * offset after Visible Envelope are removed a bit cumbersome. To make
         * this process a bit easier, we augment the header to include offsets
         * relative to the start of the file.
         *
         * lastbyte is the offset of the last byte contained in the current VR,
         * relative to the start of the file, as if there were no VE's.
         */
        std::int64_t lastbyte = 0;

        /*
         * Reflects the *actual* number of bytes in the Visible Envelope. That
         * is, the sum of the length, format and major fields.
         */
        static constexpr const int size = 4;
    };

    unique_lfp fp;
    std::vector< header > markers;
    struct cursor : public std::vector< header >::const_iterator {
        using std::vector< header >::const_iterator::const_iterator;

        std::int64_t remaining = 0;
    };

    cursor current;

    std::int64_t readinto(void*, std::int64_t) noexcept (false);
    void read_header() noexcept (false);
    void read_header(cursor) noexcept (false);
    void append(const header&) noexcept (false);
    void seek_with_index(std::int64_t) noexcept (false);
};

rp66::rp66(lfp_protocol* f) : fp(f) {
    /*
     * The real risks here is that the I/O device is *very* slow or blocked,
     * and won't yield the 4 first bytes, but instead something less. This is
     * currently not handled here, nor in the read_sul/read_header, but the
     * chance of it happening it he real world is quite slim.
     *
     * TODO: Should inspect error code, and determine if there's something to
     * do or more accurately report, rather than just 'failure'. At the end of
     * the day, though, the only way to properly determine what's going on is
     * to interrogate the underlying handle more thoroughly.
     */
    this->read_header();
}

void rp66::close() noexcept (false) {
    assert(this->fp);
    this->fp.close();
}

lfp_status rp66::readinto(
        void* dst,
        std::int64_t len,
        std::int64_t* bytes_read)
noexcept (false) {
    //TODO: do we need this restriction?
    if (std::numeric_limits< std::uint32_t>::max() < len)
        throw invalid_args("len > uint32_max");

    const auto n = this->readinto(dst, len);
    assert(n <= len);

    if (bytes_read) *bytes_read = n;
    if (n < len)    return LFP_OKINCOMPLETE;
    return LFP_OK;
}

int rp66::eof() const noexcept (true) {
    assert(not this->markers.empty());
    /*
     * There is no trailing header information. I.e. the end of the last
     * Visible Record *should* align with EOF from the underlying file handle.
     * If not, the VR is either truncated or there are some garbage bytes at
     * the end.
     */
    return this->fp->eof();
}

std::int64_t rp66::tell() const noexcept (true) {
    return this->current->lastbyte - this->current.remaining;
}

void rp66::seek(std::int64_t n) noexcept (false) {
    if (n < 0)
        throw invalid_args("seek offset n < 0");

    if (this->markers.empty())
        this->read_header();

    /*
     * Have we already index'd the right section? If so, use it and seek there.
     */
    this->current = std::prev(this->markers.end());
    if (n <= this->current->lastbyte ) {
        return this->seek_with_index(n);
    }
    /*
     * target is past the already-index'd records, so follow the headers, and
     * index them as we go
     */

    std::int64_t real_offset = (this->markers.size() * header::size)
                             +  this->current->lastbyte;

    while (true) {
        this->fp->seek( real_offset );
        this->read_header();
        real_offset += this->current->length;
        if ( n <= this->current->lastbyte ) break;
    }

    const auto remaining = this->current->lastbyte - n;
    real_offset -= remaining;
    this->fp->seek( real_offset );
    this->current.remaining = remaining;
}

std::int64_t rp66::readinto(void* dst, std::int64_t len) noexcept (false) {
    assert(this->current.remaining >= 0);
    assert(not this->markers.empty());
    std::int64_t bytes_read = 0;

    while (true) {
        if (this->eof())
            return bytes_read;
        if (this->current.remaining == 0) {
            this->read_header(this->current);

            /* might be EOF, or even empty records, so re-start  */
            continue;
        }

        assert(this->current.remaining >= 0);
        std::int64_t n;
        const auto to_read = std::min(len, this->current.remaining);
        const auto err = this->fp->readinto(dst, to_read, &n);
        assert(err == LFP_OKINCOMPLETE ? (n < to_read) : true);

        this->current.remaining -= n;
        bytes_read      += n;
        dst = advance(dst, n);

        if (err == LFP_OKINCOMPLETE)
            return bytes_read;

        assert(err == LFP_OK);

        if (n == len)
            return bytes_read;
        /*
         * The full read was performed, but there's still more requested - move
         * onto the next segment. This differs from when read returns OKINCOMPLETE,
         * in which case the underlying stream is temporarily exhausted or blocked,
         * and fewer bytes than requested could be provided.
         */

        len -= n;
    }
}

void rp66::read_header() noexcept (false) {
    assert(this->current     == this->markers.end() or
           this->current + 1 == this->markers.end());

    std::int64_t n;
    unsigned char b[header::size];
    auto err = this->fp->readinto(b, sizeof(b), &n);
    switch (err) {
        case LFP_OK: break;

        case LFP_OKINCOMPLETE:
            if (this->fp->eof()) {
                const auto msg = "rp66: unexpected EOF when reading header "
                                 "- got {} bytes";
                throw protocol_fatal(fmt::format(msg, n));
            }
            throw protocol_failed_recovery(
                "rp66: incomplete read of visible envelope, "
                "recovery not implemented"
            );
        default:
            throw not_implemented(
                "rp: unhandled error code in read_header"
            );
    }

    // Check the makefile-provided IS_BIG_ENDIAN, or the one set by gcc
    #if (IS_BIG_ENDIAN || __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        std::reverse(b + 0, b + 2);
    #endif

    header head;

    std::memcpy(&head.length, b + 0, sizeof(head.length));
    std::memcpy(&head.format, b + 2, sizeof(head.format));
    std::memcpy(&head.major,  b + 3, sizeof(head.major));

    assert(head.format == (char)0xFF);
    assert(head.major  == 1);

    std::int64_t lastbyte = head.length - header::size;
    if ( this->markers.size() )
        lastbyte += std::prev(this->markers.end())->lastbyte;

    head.lastbyte = lastbyte;

    this->append(head);
    this->current = std::prev(this->markers.end());
    this->current.remaining = head.length - header::size;
}


void rp66::read_header(cursor cur) noexcept (false) {
    // TODO: Make this a runtime check?
    assert(this->current.remaining == 0);

    if (std::next(cur) == std::end(this->markers)) {
        return this->read_header();
    }

    /*
     * The record *has* been index'd, so just reposition the underlying stream
     * and update the internal state
     */
    const cursor beg = this->markers.begin();
    this->current = std::next(cur);
    const auto rec = std::distance(beg, std::next(this->current));

    const auto tell = (rec * header::size) + cur->lastbyte;
    this->fp->seek(tell);
    this->current.remaining = this->current->length - header::size;
}

void rp66::seek_with_index(std::int64_t n) noexcept (false) {
    std::int64_t records = 1;

    this->current = this->markers.begin();
    while ( this->current->lastbyte < n ) {
        records++;
        this->current++;
    }

    const std::int64_t real_offset = (records * header::size) + n;

    this->fp->seek(real_offset);
    this->current.remaining = this->current->lastbyte - n;
}

void rp66::append(const header& head) noexcept (false) try {
    const auto size = std::int64_t(this->markers.size());
    const auto n = std::max(size - 1, std::int64_t(0));
    this->markers.push_back(head);
    this->current = this->markers.begin() + n;
} catch (...) {
    throw runtime_error("tapeimage: unable to store header");
}

}

}

lfp_protocol* lfp_rp66_open(lfp_protocol* f) {
    if (not f) return nullptr;

    try {
        return new lfp::rp66(f);
    } catch (...) {
        return nullptr;
    }
}
