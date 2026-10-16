#pragma once
// Binance USD-M futures market data: top-of-book updates (bookTicker) and aggregated
// trades (aggTrades), merged into one time-ordered event stream.
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace mm {

struct Quote {
    std::int64_t ts_ms;
    std::int64_t bid;  // price in ticks
    std::int64_t ask;
    double bid_qty;
    double ask_qty;
};

struct Trade {
    std::int64_t ts_ms;
    std::int64_t price;  // ticks
    double qty;
    bool buyer_is_maker;  // true: the seller was the aggressor, so the trade hit the bid
};

struct Event {
    enum class Kind { Quote, Trade } kind;
    Quote quote;
    Trade trade;
    std::int64_t ts() const { return kind == Kind::Quote ? quote.ts_ms : trade.ts_ms; }
};

std::int64_t to_ticks(double price, double tick);

// Parses one CSV data line. Returns nullopt for the header or a malformed line.
std::optional<Quote> parse_book_ticker(std::string_view line, double tick);
std::optional<Trade> parse_agg_trade(std::string_view line, double tick);

// Reads lines from a CSV file, or from a .zip through `unzip -p`.
class LineReader {
public:
    explicit LineReader(const std::string& path);
    ~LineReader();
    LineReader(const LineReader&) = delete;
    LineReader& operator=(const LineReader&) = delete;
    bool next(std::string_view& line);

private:
    std::FILE* f_ = nullptr;
    bool pipe_ = false;
    char* buf_ = nullptr;
    std::size_t cap_ = 0;
};

// Merges both files by exchange timestamp. At equal timestamps trades come first,
// because the quote update at that time already reflects the trade.
class EventStream {
public:
    EventStream(const std::string& book_path, const std::string& trades_path, double tick);
    bool next(Event& ev);

private:
    void fill_quote();
    void fill_trade();
    LineReader book_, trades_;
    double tick_;
    std::optional<Quote> q_;
    std::optional<Trade> t_;
};

}  // namespace mm
