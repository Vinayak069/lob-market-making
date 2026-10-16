#include "mm/market_data.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <sys/types.h>

namespace mm {

std::int64_t to_ticks(double price, double tick) { return std::llround(price / tick); }

namespace {

// Splits the next comma-separated field off `s`.
std::string_view field(std::string_view& s) {
    auto pos = s.find(',');
    std::string_view f = s.substr(0, pos);
    s = pos == std::string_view::npos ? std::string_view{} : s.substr(pos + 1);
    return f;
}

template <class T>
bool num(std::string_view f, T& out) {
    auto [p, ec] = std::from_chars(f.data(), f.data() + f.size(), out);
    return ec == std::errc{} && p == f.data() + f.size();
}

}  // namespace

std::optional<Quote> parse_book_ticker(std::string_view line, double tick) {
    // update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,transaction_time,event_time
    std::int64_t id, ts;
    double bp, bq, ap, aq;
    if (!num(field(line), id) || !num(field(line), bp) || !num(field(line), bq) || !num(field(line), ap) ||
        !num(field(line), aq) || !num(field(line), ts))
        return std::nullopt;
    return Quote{ts, to_ticks(bp, tick), to_ticks(ap, tick), bq, aq};
}

std::optional<Trade> parse_agg_trade(std::string_view line, double tick) {
    // agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,is_buyer_maker
    std::int64_t id, first, last, ts;
    double px, qty;
    if (!num(field(line), id) || !num(field(line), px) || !num(field(line), qty) || !num(field(line), first) ||
        !num(field(line), last) || !num(field(line), ts))
        return std::nullopt;
    std::string_view maker = field(line);
    if (!maker.empty() && maker.back() == '\r') maker.remove_suffix(1);
    if (maker != "true" && maker != "false") return std::nullopt;
    return Trade{ts, to_ticks(px, tick), qty, maker == "true"};
}

LineReader::LineReader(const std::string& path) {
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".zip") == 0) {
        f_ = popen(("unzip -p '" + path + "'").c_str(), "r");
        pipe_ = true;
    } else {
        f_ = std::fopen(path.c_str(), "r");
    }
    if (!f_) throw std::runtime_error("cannot open " + path);
}

LineReader::~LineReader() {
    if (f_) pipe_ ? pclose(f_) : std::fclose(f_);
    std::free(buf_);
}

bool LineReader::next(std::string_view& line) {
    ssize_t n = getline(&buf_, &cap_, f_);
    if (n < 0) return false;
    while (n > 0 && (buf_[n - 1] == '\n' || buf_[n - 1] == '\r')) --n;
    line = std::string_view(buf_, static_cast<std::size_t>(n));
    return true;
}

EventStream::EventStream(const std::string& book_path, const std::string& trades_path, double tick)
    : book_(book_path), trades_(trades_path), tick_(tick) {
    fill_quote();
    fill_trade();
}

void EventStream::fill_quote() {
    q_.reset();
    std::string_view line;
    while (!q_ && book_.next(line)) q_ = parse_book_ticker(line, tick_);
}

void EventStream::fill_trade() {
    t_.reset();
    std::string_view line;
    while (!t_ && trades_.next(line)) t_ = parse_agg_trade(line, tick_);
}

bool EventStream::next(Event& ev) {
    if (!q_ && !t_) return false;
    if (t_ && (!q_ || t_->ts_ms <= q_->ts_ms)) {
        ev.kind = Event::Kind::Trade;
        ev.trade = *t_;
        fill_trade();
    } else {
        ev.kind = Event::Kind::Quote;
        ev.quote = *q_;
        fill_quote();
    }
    return true;
}

}  // namespace mm
