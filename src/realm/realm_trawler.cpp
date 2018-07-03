/*
 * Usage: realm-trawler <realm-file-name>
 *
 * This tool will dump the structure of a realm file and print out any inconsistencies it finds.
 *
 * First it will print out information found in the top group. If there are inconsistencies in the
 * free list, this will be reported.
 *
 * Next, it will go through all tables and print the name, type and primary structure of the columns
 * found in the table. The user data found in the tables will not be interpreted.
 *
 * Generally all references will be checked in the sense that they should point to something that has
 * a valid header, meaning that the header must have a valid signature. Also, references that point
 * to areas included in the free list will be considered invalid. References that are not valid
 * will not be followed. It is checked that an area is only referenced once.
 *
 * Lastly it is checked that all space is accounted for. The combination of the free list and the
 * table tree should cover the whole file. Any leaked areas are reported.
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <set>
#include <map>
#include <memory>
#include <algorithm>
#include <cassert>
#include <cstring>
#include "array_direct.hpp"

constexpr const int signature = 0x41414141;
constexpr const int alt_signature = 41414141;
constexpr const char* err_txt = "*** Error: ";

struct Header {
    uint64_t m_top_ref[2]; // 2 * 8 bytes
    // Info-block 8-bytes
    uint8_t m_mnemonic[4];    // "T-DB"
    uint8_t m_file_format[2]; // See `library_file_format`
    uint8_t m_reserved;
    // bit 0 of m_flags is used to select between the two top refs.
    uint8_t m_flags;
};

struct StreamingFooter {
    uint64_t m_top_ref;
    uint64_t m_magic_cookie;
};

static const uint_fast64_t footer_magic_cookie = 0x3034125237E526C8ULL;

#ifdef REALM_DEBUG
namespace realm {
namespace util {
void terminate(const char* message, const char* file, long line, std::initializer_list<Printable>&&) noexcept
{
    std::cerr << file << ":" << line << ": " << message << std::endl;
    exit(1);
}
}
}
#endif

template <class T>
void consolidate_list(std::vector<T>& list)
{
    if (list.size() > 1) {
        std::sort(begin(list), end(list), [](T& a, T& b) { return a.start < b.start; });

        // Combine any adjacent chunks in the freelist, except for when the chunks
        // are on the edge of an allocation slab
        auto prev = list.begin();
        for (auto it = list.begin() + 1; it != list.end(); ++it) {
            if (prev->start + prev->length != it->start) {
                REALM_ASSERT(prev->start + prev->length < it->start);
                prev = it;
                continue;
            }

            prev->length += it->length;
            it->length = 0;
        }

        // Remove all of the now zero-size chunks from the free list
        list.erase(std::remove_if(begin(list), end(list), [](T& chunk) { return chunk.length == 0; }), end(list));
    }
}

#if 0
class DbEntry {
public:
    DbEntry() {}
    DbEntry(std::ifstream& is, uint64_t ref, std::set<Entry>& refs);

    bool is_valid() { return m_is_valid; }

    bool is_duplicate() { return m_is_duplicate; }

    bool is_inner_node() { return m_is_inner_node; }

    std::vector<uint64_t> get_refs()
    {
        return m_refs;
    }

    size_t get_size() { return m_size; }

    size_t get_byte_size() { return m_byte_size; }

    unsigned get_width() { return m_width; }

    int64_t get_val(unsigned ndx)
    {
        int64_t val = realm::get_direct(m_data.get(), m_width, ndx);

        if (m_has_refs) {
            if (val & 1) {
                val >>= 1;
            }
        }
        return val;
    }

    const char* get_data() { return m_data.get(); }

    static unsigned calc_byte_size(unsigned wtype, unsigned size, unsigned width) noexcept
    {
        unsigned num_bytes = 0;
        switch (wtype) {
            case 0: {
                unsigned num_bits = size * width;
                num_bytes = (num_bits + 7) >> 3;
                break;
            }
            case 1: {
                num_bytes = size * width;
                break;
            }
            case 2:
                num_bytes = size;
                break;
        }

        // Ensure 8-byte alignment
        return (num_bytes + 7) & ~size_t(7);
    }

private:
    bool m_is_valid = false;
    bool m_is_duplicate = false;
    bool m_has_refs = false;
    bool m_is_inner_node = false;
    unsigned m_size = 0;
    unsigned m_byte_size = 0;
    unsigned m_width = 0;
    std::vector<uint64_t> m_refs;
    std::unique_ptr<char> m_data;

    void get_refs_from_buffer()
    {
        for (unsigned i = 0; i < m_size; i++) {
            int64_t val = realm::get_direct(m_data.get(), m_width, i);
            if (val && !(val & 1)) {
                m_refs.push_back(uint64_t(val));
            }
        }
    }
};

class RealmFile {
public:
    RealmFile(std::ifstream& is);
    void check_leaked();

private:
    std::ifstream& m_is;
    uint64_t m_top_ref;

    void process_group(uint64_t ref);
    std::vector<std::string> process_names(uint64_t ref);
    std::vector<uint64_t> process_numbers(uint64_t ref);
    std::vector<std::string> process_types(uint64_t ref);
    void process_free_list(uint64_t pos_ref, uint64_t size_ref, uint64_t version_ref);
    void process_history(uint64_t ref);
    void process_spec(uint64_t ref, std::string table_name, std::vector<uint64_t>& column_refs);
    void process_table(DbEntry& db_entry, std::string name);
    void add_ref(uint64_t ref);
    void add_column_ref(uint64_t ref, std::string lead);
    std::vector<Entry> check_refs();
};

uint64_t suspicious_ref;

DbEntry::DbEntry(std::ifstream& is, uint64_t ref, std::set<Entry>& refs)
{
    unsigned char header[8];
    is.seekg (ref, is.beg);
    is.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (memcmp(header, &signature, 4) && memcmp(header, &alt_signature, 4)) {
        auto it = free_list.lower_bound(Entry(ref, 0));
        if (it != free_list.begin()) {
            it--;
            if (ref > it->start && ref < it->start + it->length) {
                std::cerr << err_txt << "Invalid ref in free space: 0x" << std::hex << ref << std::dec << std::endl;
            }
            else {
                std::cerr << err_txt << "Invalid ref: 0x" << std::hex << ref << std::dec << std::endl;
            }
        }
        if (!is) {
            is.clear();
        }
    }
    else {
        m_width = (1 << (unsigned(header[4]) & 0x07)) >> 1;
        unsigned width_type = (unsigned(header[4]) & 0x18) >> 3;
        m_size = header[5];
        m_size = (m_size << 8) + header[6];
        m_size = (m_size << 8) + header[7];

        m_byte_size = calc_byte_size(width_type, m_size, m_width);
        m_data.reset(new char[m_byte_size]);

        is.read(m_data.get(), m_byte_size);

        if (header[4] & 0x80) {
            m_is_inner_node = true;
        }
        if (header[4] & 0x40) {
            m_has_refs = true;
            get_refs_from_buffer();
        }
        Entry entry(ref, m_byte_size);
        if (free_list.count(entry)) {
            std::cerr << err_txt << "Ref found in free list: 0x" << std::hex << ref << std::dec << std::endl;
        }
        else {
            m_is_valid = true;
            auto res = refs.emplace(ref, m_byte_size + 8);
            m_is_duplicate = !res.second;
            if (m_is_duplicate) {
                std::cerr << err_txt << "Duplicate ref 0x" << std::hex << ref << std::dec << std::endl;
            }
        }
    }
}

void RealmFile::add_ref(uint64_t ref)
{
    DbEntry db_entry(m_is, ref, m_refs);
    if (db_entry.is_valid() && !db_entry.is_duplicate()) {
        auto sub_refs = db_entry.get_refs();
        for (unsigned i = 0; i < sub_refs.size(); i++) {
            add_ref(sub_refs[i]);
        }
    }
}

void RealmFile::add_column_ref(uint64_t ref, std::string lead)
{
    DbEntry db_entry(m_is, ref, m_refs);
    if (db_entry.is_valid() && !db_entry.is_duplicate()) {
        auto sub_refs = db_entry.get_refs();
        /*
                if (db_entry.is_inner_node()) {
                    std::cout << lead << "inner node with " << sub_refs.size() << " children" << std::endl;
                }
                else {
                    if (!sub_refs.empty()) {
                        std::cout << lead << "leaf with " << sub_refs.size() << " children" << std::endl;
                    }
                    else {
                        std::cout << lead << "leaf size " << db_entry.get_size() << std::endl;
                    }
                }
        */
        for (unsigned i = 0; i < sub_refs.size(); i++) {
            add_column_ref(sub_refs[i], lead + "    ");
        }
    }
}

std::vector<std::string> RealmFile::process_names(uint64_t ref)
{
    std::vector<std::string> names;
    DbEntry db_entry(m_is, ref, m_refs);
    if (db_entry.is_valid()) {
        auto sz = db_entry.get_byte_size();
        auto width = db_entry.get_width();
        const char* buffer = db_entry.get_data();

        for (unsigned s = 0; s < sz; s += width) {
            names.push_back(buffer + s);
        }
    }
    return names;
}

static std::string columnType_to_str(int t)
{
    switch (t) {
    // Column types
    case  0: return "Int";
    case  1: return "Bool";
    case  2: return "String";
    case  3: return "StringEnum";
    case  4: return "Binary";
    case  5: return "Table";
    case  6: return "Mixed";
    case  7: return "OldDateTime";
    case  8: return "Timestamp";
    case  9: return "Float";
    case 10: return "Double";
    case 11: return "Reserved4";
    case 12: return "Link";
    case 13: return "LinkList";
    case 14: return "BackLink";
    }
    return "Invalid";
}

std::vector<uint64_t> RealmFile::process_numbers(uint64_t ref)
{
    std::vector<uint64_t> numbers;
    DbEntry db_entry(m_is, ref, m_refs);
    if (db_entry.is_valid()) {
        auto sz = db_entry.get_size();
        for (unsigned s = 0; s < sz; s++) {
            numbers.push_back(db_entry.get_val(s));
        }
    }
    return numbers;
}

std::vector<std::string> RealmFile::process_types(uint64_t ref)
{
    std::vector<std::string> type_names;
    DbEntry db_entry(m_is, ref, m_refs);
    if (db_entry.is_valid()) {
        auto sz = db_entry.get_size();

        for (unsigned s = 0; s < sz; s++) {
            type_names.push_back(columnType_to_str(db_entry.get_val(s)));
        }
    }
    return type_names;
}

void RealmFile::process_free_list(uint64_t pos_ref, uint64_t size_ref, uint64_t version_ref)
{
    std::set<int> version_set;
    uint64_t newest = 0;
    auto positions = process_numbers(pos_ref);
    auto sizes = process_numbers(size_ref);
    auto versions = process_numbers(version_ref);
    if (positions.size() && sizes.size() && versions.size()) {
        auto sz = positions.size();
        assert(sz == sizes.size() && sz == versions.size());

        for (unsigned s = 0; s < sz; s++) {
            Entry free_list_entry(positions[s], sizes[s], versions[s]);
            auto it = free_list.find(free_list_entry);
            if (it != free_list.end()) {
                std::cerr << err_txt << "Multiple free list entry:" << std::endl;
                std::cerr << err_txt << "    " << *it << std::endl;
                std::cerr << err_txt << "    " << free_list_entry << std::endl;
                // Copy and remove duplicate
                Entry duplicate = *it;
                free_list.erase(it);
                // Insert biggest
                if (duplicate.length > free_list_entry.length) {
                    free_list.insert(duplicate);
                }
                else {
                    free_list.insert(free_list_entry);
                }
            }
            else {
                free_list.insert(free_list_entry);

                version_set.insert(free_list_entry.version);
                if (free_list_entry.version > newest) {
                    newest = free_list_entry.version;
                }
            }
        }

        std::cout << "number of versions: " << version_set.size() << std::endl;
        std::cout << "last version: " << newest << std::endl;
    }
}

void RealmFile::process_history(uint64_t ref)
{
    DbEntry hist(m_is, ref, m_refs);
    if (hist.is_valid()) {
        std::cout << "Change log:" << std::endl;
        auto hist_ref = hist.get_refs();
        for (auto r : hist_ref) {
            DbEntry blob(m_is, r, m_refs);
            auto sz = blob.get_size();
            const unsigned char* buffer = reinterpret_cast<const unsigned char*>(blob.get_data());

            std::cout << "    ";
            for (unsigned i = 0; i < sz; i++) {
                printf("%02x ", buffer[i]);
            }
            std::cout << std::endl;
        }
    }
}

void RealmFile::process_spec(uint64_t ref, std::string table_name, std::vector<uint64_t>& column_refs)
{
    DbEntry spec(m_is, ref, m_refs);
    if (spec.is_valid()) {
        if (spec.get_size() > 0) {
            std::unique_ptr<DbEntry> sub_spec;
            unsigned spec_ndx = 0;
            auto type_names = process_types(spec.get_val(0));
            auto column_names = process_names(spec.get_val(1));
            auto attributes = process_numbers(spec.get_val(2));

            if (spec.get_size() > 3) {
                sub_spec = std::make_unique<DbEntry>(m_is, spec.get_val(3), m_refs);
                auto sub_spec_refs = sub_spec->get_refs();
                for (auto r : sub_spec_refs) add_ref(r);
            }

            if (spec.get_size() > 4) {
                add_ref(spec.get_val(4));
            }
            std::cout << table_name << std::endl;
            unsigned col_ndx = 0;
            for (unsigned i = 0; i < column_names.size(); i++) {
                std::cout << "   " << i << ": " << column_names[i] << " - "<< type_names[i] << std::endl;
                if (column_refs.size()) {
                    add_column_ref(column_refs[col_ndx++], "    ");
                    if (attributes[i] & 1) {
                        std::cout << "      Indexed" << std::endl;
                        add_ref(column_refs[col_ndx++]);
                    }
                }
                if ((type_names[i] == "Link" || type_names[i] == "LinkList") && sub_spec) {
                    std::cout << "      Target table: " << sub_spec->get_val(spec_ndx++) << std::endl;
                }
            }
            if (sub_spec) {
                while (col_ndx < column_refs.size()) {
                    std::cout << "   " << "Backlink: Origin table: " << sub_spec->get_val(spec_ndx) << " Origin column: " << sub_spec->get_val(spec_ndx+1) << std::endl;
                    spec_ndx += 2;
                    add_ref(column_refs[col_ndx++]);
                }
            }
        }
    }
}

void RealmFile::process_table(DbEntry& table, std::string name)
{
    if (table.is_valid()) {
        assert(table.get_size() == 2);
        uint64_t spec_ref = table.get_val(0);
        uint64_t column_ref = table.get_val(1);

        std::vector<uint64_t> column_refs;
        DbEntry columns(m_is, column_ref, m_refs);
        if (columns.is_valid()) {
            auto sz = columns.get_size();
            for (unsigned i = 0; i < sz; i++) {
                column_refs.push_back(columns.get_val(i));
            }
        }
        process_spec(spec_ref, name, column_refs);
    }
}

void RealmFile::process_group(uint64_t ref)
{
    DbEntry group(m_is, ref, m_refs);
    if (group.is_valid()) {
        std::cout << "File size: " << group.get_val(2) << std::endl;
        std::cout << "Current version: " << group.get_val(6) << std::endl;

        process_free_list(group.get_val(3), group.get_val(4), group.get_val(5));
        process_history(group.get_val(8));

        auto table_names = process_names(group.get_val(0));

        std::vector<DbEntry> table_entries;
        DbEntry tables(m_is, group.get_val(1), m_refs);
        for (unsigned i = 0; i < tables.get_size(); i++) {
            table_entries.emplace_back(m_is, tables.get_val(i), m_refs);
        }
        for (unsigned i = 0; i < table_names.size(); i++) {
            std::cout << "\nTable " << i << std::endl;
            process_table(table_entries[i], table_names[i]);
        }
    }
}

std::vector<Entry> RealmFile::check_refs()
{
    std::vector<Entry> leaked;

    unsigned start = sizeof(Header);
    std::vector<Entry> combined(m_refs.size() + free_list.size());
    auto it = std::set_union(m_refs.begin(), m_refs.end(), free_list.begin(), free_list.end(), combined.begin());
    combined.resize(it - combined.begin());
    Entry previous(0,0);
    for (auto& a : combined) {
        if (previous.start + previous.length > a.start) {
            std::cerr << err_txt << "Overlapping area:" << std::endl;
            std::cerr << err_txt << "    " << previous << std::endl;
            std::cerr << err_txt << "    " << a << std::endl;
        }
        if (a.start > start) {
            Entry e(start, a.start - start);
            leaked.push_back(e);
        }
        start = a.start + a.length;
        previous = a;
    }
    return leaked;
}

void RealmFile::check_leaked()
{
    std::vector<Entry> leaked = check_refs();
    for (auto& a : leaked) {
        std::cerr << err_txt << std::hex << "Leaked space: pos: 0x" << a.start << ", size: 0x" << a.length << std::dec
                  << std::endl;
        std::set<Entry> local_refs;
        unsigned start = a.start;
        unsigned end = a.start + a.length;
        while (start < end) {
            DbEntry db_entry(m_is, start, local_refs);
            start += 8;
            if (db_entry.is_valid()) {
                start += db_entry.get_byte_size();
            }
        }

        for (auto& r : local_refs) {
            auto it = m_refs.find(r);
            if (it == m_refs.end()) {
                std::cerr << err_txt << "   Found ref: " << r << std::endl;
            }
        }
    }
}
#endif

struct Entry {
    Entry()
        : Entry(0, 0)
    {
    }
    Entry(uint64_t s, uint64_t l)
        : start(s)
        , length(l)
    {
    }
    bool operator<(const Entry& rhs) const
    {
        return start < rhs.start;
    }
    uint64_t start;
    uint64_t length;
};

struct FreeListEntry : public Entry {
    FreeListEntry()
        : FreeListEntry(0, 0)
    {
    }
    FreeListEntry(uint64_t s, uint64_t l, uint64_t v = 0)
        : Entry(s, l)
        , version(v)
    {
    }
    uint64_t version;
};

class Node {
public:
    Node()
    {
    }
    Node(std::ifstream& is, uint64_t ref)
    {
        init(is, ref);
    }
    void init(std::ifstream& is, uint64_t ref);
    bool valid() const
    {
        return m_valid;
    }
    bool has_refs() const
    {
        return (m_header[4] & 0x40) != 0;
    }
    unsigned width() const
    {
        return (1 << (unsigned(m_header[4]) & 0x07)) >> 1;
    }
    unsigned size() const
    {
        return unsigned(m_size);
    }
    unsigned length() const
    {
        unsigned width_type = (unsigned(m_header[4]) & 0x18) >> 3;
        return calc_byte_size(width_type, m_size, width());
    }
    uint64_t size_in_bytes() const
    {
        return sizeof(m_header) + length();
    }

protected:
    unsigned char m_header[8];
    unsigned m_size = 0;
    bool m_valid = false;

    static unsigned calc_byte_size(unsigned wtype, unsigned size, unsigned width)
    {
        unsigned num_bytes = 0;
        switch (wtype) {
            case 0: {
                unsigned num_bits = size * width;
                num_bytes = (num_bits + 7) >> 3;
                break;
            }
            case 1: {
                num_bytes = size * width;
                break;
            }
            case 2:
                num_bytes = size;
                break;
        }

        // Ensure 8-byte alignment
        return (num_bytes + 7) & ~size_t(7);
    }
};

class Array : public Node {
public:
    Array()
    {
    }
    Array(std::ifstream& is, uint64_t ref)
        : Node(is, ref)
    {
        init(is, ref);
    }
    void init(std::ifstream& is, uint64_t ref)
    {
        Node::init(is, ref);
        unsigned len = length();
        m_data.reset(new char[len]);
        is.read(m_data.get(), len);
        m_has_refs = has_refs();
    }
    int64_t get_val(unsigned ndx) const
    {
        int64_t val = realm::get_direct(m_data.get(), width(), ndx);

        if (m_has_refs) {
            if (val & 1) {
                val >>= 1;
            }
        }
        return val;
    }
    uint64_t get_ref(unsigned ndx) const
    {
        REALM_ASSERT(m_has_refs);
        int64_t val = realm::get_direct(m_data.get(), width(), ndx);

        if (val & 1)
            val = 0;

        return uint64_t(val);
    }
    std::string get_string(unsigned ndx) const
    {
        std::string str;
        if (valid()) {
            auto sz = size_in_bytes();
            auto w = width();
            REALM_ASSERT(ndx * w < sz);
            str = m_data.get() + ndx * w;
        }
        return str;
    }

    static void get_nodes(std::ifstream& is, uint64_t ref, std::vector<Entry>&);

private:
    std::unique_ptr<char> m_data;
    bool m_has_refs = false;
};

class Group : public Array {
public:
    Group(std::ifstream& is, uint64_t ref)
        : Array(is, ref)
        , m_is(is)
    {
        if (valid()) {
            m_table_names.init(is, get_ref(0));
            m_file_size = get_val(2);
            m_free_list_positions.init(is, get_ref(3));
            m_free_list_sizes.init(is, get_ref(4));
            m_free_list_versions.init(is, get_ref(5));
        }
    }
    uint64_t get_file_size() const
    {
        return m_file_size;
    }
    int get_current_version() const
    {
        return int();
    }
    int get_nb_tables() const
    {
        return m_table_names.size();
    }
    void get_table_nodes(size_t ndx, std::vector<Entry>& nodes) const;
    std::vector<Entry> get_allocated_nodes() const;
    std::vector<FreeListEntry> get_free_list() const;

private:
    friend std::ostream& operator<<(std::ostream& ostr, const Group& g);
    std::ifstream& m_is;
    uint64_t m_file_size;
    Array m_table_names;
    Array m_free_list_positions;
    Array m_free_list_sizes;
    Array m_free_list_versions;
};

std::ostream& operator<<(std::ostream& ostr, const Group& g)
{
    if (g.valid()) {
        ostr << "File size: " << g.get_file_size() << std::endl;
        ostr << "Current version:" << g.get_current_version() << std::endl;
        ostr << "Free list size: " << g.m_free_list_positions.size() << std::endl;
        ostr << "Tables: " << std::endl;

        for (int i = 0; i < g.get_nb_tables(); i++) {
            std::cout << "    " << g.m_table_names.get_string(i) << std::endl;
        }
    }
    else {
        ostr << "Invalid group" << std::endl;
    }
    return ostr;
}

class RealmFile {
public:
    RealmFile(std::ifstream& is);
    // Walk the file and check that it consists of valid nodes
    void node_scan();
    void memory_leaks();
    void free_list_info() const;

private:
    std::ifstream& m_is;
    uint64_t m_top_ref;
    uint64_t m_start_pos;
    int m_file_format_version;
    std::unique_ptr<Group> m_group;
};

void Node::init(std::ifstream& is, uint64_t ref)
{
    unsigned char header[8];
    is.seekg(ref, is.beg);
    is.read(reinterpret_cast<char*>(&m_header), sizeof(header));

    if (memcmp(m_header, &signature, 4)) {
    }
    else {
        m_size = (m_header[5] << 16) + (m_header[6] << 8) + m_header[7];
        m_valid = true;
    }
}

void Array::get_nodes(std::ifstream& is, uint64_t ref, std::vector<Entry>& nodes)
{
    Array arr(is, ref);
    nodes.emplace_back(ref, arr.size_in_bytes());
    if (arr.has_refs()) {
        auto sz = arr.size();
        for (size_t i = 0; i < sz; i++) {
            uint64_t r = arr.get_ref(i);
            if (r)
                get_nodes(is, r, nodes);
        }
    }
}

void Group::get_table_nodes(size_t ndx, std::vector<Entry>& nodes) const
{
    Array table_refs(m_is, get_ref(1));
    auto ref = table_refs.get_val(ndx);

    Array::get_nodes(m_is, ref, nodes);

    consolidate_list(nodes);
}

std::vector<Entry> Group::get_allocated_nodes() const
{
    std::vector<Entry> all_nodes;

    Array::get_nodes(m_is, get_ref(0), all_nodes); // Table names

    for (int i = 0; i < get_nb_tables(); i++) {
        get_table_nodes(i, all_nodes);
    }
    if (size() > 8) {
        Array::get_nodes(m_is, get_ref(8), all_nodes);
    }

    consolidate_list(all_nodes);
    return all_nodes;
}

std::vector<FreeListEntry> Group::get_free_list() const
{
    std::vector<FreeListEntry> list;
    size_t sz = m_free_list_positions.size();
    REALM_ASSERT(sz == m_free_list_sizes.size());
    REALM_ASSERT(sz == m_free_list_versions.size());
    for (size_t i = 0; i < sz; i++) {
        int64_t pos = m_free_list_positions.get_val(i);
        int64_t size = m_free_list_sizes.get_val(i);
        int64_t version = m_free_list_versions.get_val(i);
        list.emplace_back(pos, size, version);
    }
    consolidate_list(list);
    return list;
}

RealmFile::RealmFile(std::ifstream& is)
    : m_is(is)
{
    m_top_ref = 0;
    union Buffer {
        Header file_header;
        StreamingFooter file_footer;
        char plain[sizeof(Header)];
    } buffer;

    is.seekg (0);
    is.read(buffer.plain, sizeof(Header));
    if (is) {
        if (std::memcmp(buffer.file_header.m_mnemonic, "T-DB", 4)) {
            std::cerr << err_txt << "Not a realm file ?" << std::endl;
            return;
        }
        else {
            m_top_ref = buffer.file_header.m_top_ref[buffer.file_header.m_flags];
            m_start_pos = sizeof(Header);
            if (m_top_ref == 0xFFFFFFFFFFFFFFFFULL && buffer.file_header.m_flags == 0) {
                // Streaming format
                m_start_pos = 0;
                is.seekg(0 - sizeof(StreamingFooter), is.end);
                is.read(buffer.plain, sizeof(StreamingFooter));
                if (buffer.file_footer.m_magic_cookie == footer_magic_cookie) {
                    m_top_ref = unsigned(buffer.file_footer.m_top_ref);
                }
                else {
                    std::cerr << err_txt << "Top ref not found" << std::endl;
                    return;
                }
            }
        }
    }
    m_group = std::make_unique<Group>(is, m_top_ref);
    m_file_format_version = buffer.file_header.m_file_format[buffer.file_header.m_flags];
    std::cout << "Top ref: 0x" << std::hex << m_top_ref << std::dec << std::endl;
    std::cout << "File format version: " << m_file_format_version << std::endl;
    std::cout << *m_group << std::endl;
}

void RealmFile::node_scan()
{
    if (m_group->valid()) {
        std::map<uint64_t, unsigned> sizes;
        uint64_t ref = m_start_pos;
        auto free_list = m_group->get_free_list();
        auto free_entry = free_list.begin();
        bool searching = false;
        while (ref < m_group->get_file_size()) {
            while (ref == free_entry->start) {
                ref += free_entry->length;
                ++free_entry;
            }
            Node n(m_is, ref);
            if (n.valid()) {
                if (searching) {
                    std::cerr << "Resuming from ref: " << ref << std::endl;
                    searching = false;
                }
                auto size_in_bytes = n.size_in_bytes();
                sizes[size_in_bytes]++;
                ref += size_in_bytes;
            }
            else {
                if (!searching) {
                    std::cerr << "Invalid ref: " << ref << std::endl;
                    searching = true;
                }
                ref += 8;
            }
        }
        std::cout << "Allocated space:" << std::endl;
        for (auto s : sizes) {
            std::cout << "    Size: " << s.first << " count: " << s.second << std::endl;
        }
    }
}
void RealmFile::memory_leaks()
{
    if (m_group->valid()) {
        auto nodes = m_group->get_allocated_nodes();
        auto free_list = m_group->get_free_list();
        for (auto& entry : free_list) {
            nodes.emplace_back(entry.start, entry.length);
        }
        consolidate_list(nodes);
    }
}

void RealmFile::free_list_info() const
{
    std::map<uint64_t, unsigned> free_sizes;
    std::map<uint64_t, unsigned> pinned_sizes;
    std::cout << "Free space:" << std::endl;
    auto free_list = m_group->get_free_list();
    size_t pinned_free_list_size = 0;
    size_t total_free_list_size = 0;
    auto it = free_list.begin();
    auto end = free_list.end();
    while (it != end) {
        // std::cout << it->start << ", " << it->length << ", " << it->version << std::endl;
        total_free_list_size += it->length;
        if (it->version != 0) {
            pinned_free_list_size += it->length;
            pinned_sizes[it->length]++;
        }
        else {
            free_sizes[it->length]++;
        }

        ++it;
    }
    std::cout << "Free space sizes:" << std::endl;
    for (auto s : free_sizes) {
        std::cout << "    Size: " << s.first << " count: " << s.second << std::endl;
    }
    std::cout << "Pinned sizes:" << std::endl;
    for (auto s : pinned_sizes) {
        std::cout << "    Size: " << s.first << " count: " << s.second << std::endl;
    }
    std::cout << "Total free space size:  " << total_free_list_size << std::endl;
    std::cout << "Pinned free space size: " << pinned_free_list_size << std::endl;
}

int main (int argc, const char* argv[])
{
    if (argc > 1) {
        std::ifstream is(argv[1], std::ifstream::binary);
        if (is) {
            RealmFile rf(is);
            if (argc > 2) {
                for (const char* command = argv[2]; *command != '\0'; command++) {
                    switch (*command) {
                        case 'f':
                            rf.free_list_info();
                            break;
                        case 'm':
                            rf.memory_leaks();
                            break;
                        case 'w':
                            rf.node_scan();
                            break;
                    }
                }
            }
        }
        else {
            std::cerr << err_txt << "Could not open file '" << argv[1] << "'" << std::endl;
        }
    }
    else {
        std::cerr << "Usage: realm-trawler <realmfile>" << std::endl;
    }

    return 0;
}
