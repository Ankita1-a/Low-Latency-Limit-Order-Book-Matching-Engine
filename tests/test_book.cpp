// tests/test_book.cpp
//
// Unit tests for the order book rules (see docs/DESIGN.md).
// Every test runs against every book type listed in BOOK_TYPES.

#include <cstdio>
#include <utility>   // std::move
#include <vector>

#include "doctest.h"
#include "lob/events.hpp"
#include "naive_book.hpp"

using namespace lob;

// C++20 "using enum": lets us write Buy instead of Side::Buy, etc.
// Keeps the tests short and readable.
using enum Side;
using enum OrderType;
using enum RejectReason;
using enum CancelReason;

// ---------------------------------------------------------------------
// The book types under test. Later: add the fast book to this list.
// ---------------------------------------------------------------------
using Naive = NaiveBook<RecordingListener>;
#define BOOK_TYPES Naive

// ---------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------

constexpr Price kMin = 1900;   // price band used by every test
constexpr Price kMax = 2100;

// A "fixture" bundles what every test needs: a recorder and a book.
// rec is declared first, so it exists before book is built with it.
template <class BookT>
struct Fixture {
    RecordingListener rec;
    BookT book;

    explicit Fixture(std::size_t capacity = 1000) : book(rec, kMin, kMax, capacity) {}

    // Hand over the recorded events and start fresh.
    std::vector<Event> take() {
        std::vector<Event> out = std::move(rec.events);
        rec.clear();
        return out;
    }
};

// Short factory functions so expected events fit on one line.
Event acc(OrderId id, Side s, OrderType t, Price p, Qty q) { return Accepted{id, s, t, p, q}; }
Event trade(OrderId taker, OrderId maker, Side s, Price p, Qty q) { return Trade{taker, maker, s, p, q}; }
Event cxl(OrderId id, Qty q, CancelReason r) { return Cancelled{id, q, r}; }
Event rej(OrderId id, RejectReason r) { return Rejected{id, r}; }
Event mod(OrderId id, Price p, Qty q, bool kept) { return Modified{id, p, q, kept}; }

// Compare the events since the last take() with what we expect.
// On a mismatch, print both lists so you can see exactly what differs.
template <class F>
void expect_events(F& f, const std::vector<Event>& expected) {
    const std::vector<Event> actual = f.take();
    if (actual != expected) {
        std::fprintf(stderr, "---- expected ----\n");
        for (const Event& e : expected) print_event(stderr, e);
        std::fprintf(stderr, "---- actual ----\n");
        for (const Event& e : actual) print_event(stderr, e);
    }
    CHECK(actual == expected);
}

// Check the order IDs queued at one price, front first.
template <class F>
void expect_queue(F& f, Side side, Price price, const std::vector<OrderId>& expected) {
    CHECK(f.book.queue_at(side, price) == expected);
}

// =====================================================================
// Resting and matching
// =====================================================================

TEST_CASE_TEMPLATE("non-crossing limit orders rest in the book", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Sell, Limit, 2005, 100);
    f.book.add(2, Buy,  Limit, 2000, 50);
    expect_events(f, {acc(1, Sell, Limit, 2005, 100), acc(2, Buy, Limit, 2000, 50)});
    CHECK(f.book.best_ask() == 2005);
    CHECK(f.book.best_bid() == 2000);
    CHECK(f.book.order_count() == 2);
    CHECK(f.book.volume_at(Sell, 2005) == 100);
    CHECK(f.book.volume_at(Buy, 2000) == 50);
}

TEST_CASE_TEMPLATE("trades happen at the resting (maker) price", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Sell, Limit, 2005, 100);
    f.take();
    f.book.add(2, Buy, Limit, 2010, 40);   // willing to pay 2010, gets 2005
    expect_events(f, {acc(2, Buy, Limit, 2010, 40), trade(2, 1, Buy, 2005, 40)});
    CHECK(f.book.volume_at(Sell, 2005) == 60);
    CHECK(f.book.order_count() == 1);      // buyer fully filled, didn't rest
}

TEST_CASE_TEMPLATE("time priority: first in, first filled at the same price", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Sell, Limit, 2005, 100);
    f.book.add(2, Sell, Limit, 2005, 100);
    f.take();
    f.book.add(3, Buy, Limit, 2005, 150);
    expect_events(f, {acc(3, Buy, Limit, 2005, 150),
                      trade(3, 1, Buy, 2005, 100),
                      trade(3, 2, Buy, 2005, 50)});
    expect_queue(f, Sell, 2005, {2});
    CHECK(f.book.volume_at(Sell, 2005) == 50);
}

TEST_CASE_TEMPLATE("price priority beats time priority", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Sell, Limit, 2007, 50);  // arrived first, worse price
    f.book.add(2, Sell, Limit, 2005, 50);  // arrived second, better price
    f.take();
    f.book.add(3, Buy, Limit, 2010, 50);
    expect_events(f, {acc(3, Buy, Limit, 2010, 50), trade(3, 2, Buy, 2005, 50)});
    CHECK(f.book.best_ask() == 2007);
}

TEST_CASE_TEMPLATE("limit order sweeps several levels then rests the rest", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Sell, Limit, 2005, 30);
    f.book.add(2, Sell, Limit, 2006, 30);
    f.take();
    f.book.add(3, Buy, Limit, 2006, 100);
    expect_events(f, {acc(3, Buy, Limit, 2006, 100),
                      trade(3, 1, Buy, 2005, 30),
                      trade(3, 2, Buy, 2006, 30)});
    CHECK_FALSE(f.book.best_ask().has_value());   // all asks consumed
    CHECK(f.book.best_bid() == 2006);
    CHECK(f.book.volume_at(Buy, 2006) == 40);
}

TEST_CASE_TEMPLATE("limit order never trades beyond its limit price", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Sell, Limit, 2005, 30);
    f.book.add(2, Sell, Limit, 2007, 30);
    f.take();
    f.book.add(3, Buy, Limit, 2006, 100);
    expect_events(f, {acc(3, Buy, Limit, 2006, 100), trade(3, 1, Buy, 2005, 30)});
    CHECK(f.book.best_ask() == 2007);
    CHECK(f.book.best_bid() == 2006);
    CHECK(f.book.volume_at(Buy, 2006) == 70);
}

TEST_CASE_TEMPLATE("sell orders match the highest bids first", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Buy, Limit, 2000, 50);
    f.book.add(2, Buy, Limit, 1999, 50);
    f.take();
    f.book.add(3, Sell, Limit, 1999, 80);
    expect_events(f, {acc(3, Sell, Limit, 1999, 80),
                      trade(3, 1, Sell, 2000, 50),
                      trade(3, 2, Sell, 1999, 30)});
    CHECK(f.book.best_bid() == 1999);
    CHECK(f.book.volume_at(Buy, 1999) == 20);
    CHECK_FALSE(f.book.best_ask().has_value());
}

// =====================================================================
// Order types
// =====================================================================

TEST_CASE_TEMPLATE("IOC fills what it can and cancels the rest", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Buy, Limit, 2000, 50);
    f.take();
    f.book.add(2, Sell, IOC, 2000, 80);
    expect_events(f, {acc(2, Sell, IOC, 2000, 80),
                      trade(2, 1, Sell, 2000, 50),
                      cxl(2, 30, Unfilled)});
    CHECK(f.book.order_count() == 0);

    f.book.add(3, Sell, IOC, 2050, 10);    // nothing to trade with
    expect_events(f, {acc(3, Sell, IOC, 2050, 10), cxl(3, 10, Unfilled)});
    CHECK_FALSE(f.book.best_ask().has_value());   // IOC never rests
}

TEST_CASE_TEMPLATE("market order sweeps at any price, leftover cancelled", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Sell, Limit, 2005, 30);
    f.book.add(2, Sell, Limit, 2090, 30);
    f.take();
    f.book.add(3, Buy, Market, 0, 100);
    expect_events(f, {acc(3, Buy, Market, 0, 100),
                      trade(3, 1, Buy, 2005, 30),
                      trade(3, 2, Buy, 2090, 30),
                      cxl(3, 40, Unfilled)});
    CHECK(f.book.order_count() == 0);

    f.book.add(4, Sell, Market, 0, 10);    // empty book: nothing to hit
    expect_events(f, {acc(4, Sell, Market, 0, 10), cxl(4, 10, Unfilled)});
}

TEST_CASE_TEMPLATE("FOK is all or nothing", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Buy, Limit, 2000, 50);
    f.book.add(2, Buy, Limit, 1999, 50);
    f.take();

    f.book.add(3, Sell, FOK, 1999, 101);   // only 100 available
    expect_events(f, {acc(3, Sell, FOK, 1999, 101), cxl(3, 101, Unfilled)});
    CHECK(f.book.volume_at(Buy, 2000) == 50);   // book untouched
    CHECK(f.book.volume_at(Buy, 1999) == 50);

    f.book.add(4, Sell, FOK, 2000, 60);    // only 50 at prices >= 2000
    expect_events(f, {acc(4, Sell, FOK, 2000, 60), cxl(4, 60, Unfilled)});

    f.book.add(5, Sell, FOK, 1999, 100);   // exactly enough
    expect_events(f, {acc(5, Sell, FOK, 1999, 100),
                      trade(5, 1, Sell, 2000, 50),
                      trade(5, 2, Sell, 1999, 50)});
    CHECK(f.book.order_count() == 0);
}

TEST_CASE_TEMPLATE("post-only is rejected if it would trade, rests otherwise", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Buy, Limit, 2000, 50);
    f.take();
    f.book.add(2, Sell, PostOnly, 2000, 10);   // touching the bid counts as crossing
    f.book.add(3, Sell, PostOnly, 1995, 10);   // clearly crosses
    f.book.add(4, Sell, PostOnly, 2001, 10);   // safe
    expect_events(f, {rej(2, WouldCross), rej(3, WouldCross),
                      acc(4, Sell, PostOnly, 2001, 10)});
    CHECK(f.book.best_ask() == 2001);
    CHECK(f.book.volume_at(Buy, 2000) == 50);
}

// =====================================================================
// Cancel
// =====================================================================

TEST_CASE_TEMPLATE("cancel removes the remaining quantity", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Sell, Limit, 2005, 100);
    f.book.add(2, Buy,  Limit, 2005, 30);  // partially fills #1
    f.take();
    f.book.cancel(1);
    expect_events(f, {cxl(1, 70, UserRequested)});   // 70 left, not 100
    CHECK(f.book.order_count() == 0);
    CHECK_FALSE(f.book.best_ask().has_value());

    f.book.cancel(1);      // already gone
    f.book.cancel(999);    // never existed
    expect_events(f, {rej(1, UnknownOrderId), rej(999, UnknownOrderId)});
}

TEST_CASE_TEMPLATE("cancelling from the middle keeps the queue order", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Buy, Limit, 2000, 10);
    f.book.add(2, Buy, Limit, 2000, 10);
    f.book.add(3, Buy, Limit, 2000, 10);
    f.take();
    f.book.cancel(2);
    expect_events(f, {cxl(2, 10, UserRequested)});
    expect_queue(f, Buy, 2000, {1, 3});

    f.book.add(4, Sell, Limit, 2000, 15);
    expect_events(f, {acc(4, Sell, Limit, 2000, 15),
                      trade(4, 1, Sell, 2000, 10),
                      trade(4, 3, Sell, 2000, 5)});
    expect_queue(f, Buy, 2000, {3});
}

// =====================================================================
// Modify
// =====================================================================

TEST_CASE_TEMPLATE("modify: reducing size keeps queue priority", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Buy, Limit, 2000, 50);
    f.book.add(2, Buy, Limit, 2000, 50);
    f.take();
    f.book.modify(1, 2000, 20);
    f.book.modify(1, 2000, 20);            // same size is not an increase
    expect_events(f, {mod(1, 2000, 20, true), mod(1, 2000, 20, true)});
    expect_queue(f, Buy, 2000, {1, 2});
    CHECK(f.book.volume_at(Buy, 2000) == 70);
}

TEST_CASE_TEMPLATE("modify: increasing size loses queue priority", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Buy, Limit, 2000, 50);
    f.book.add(2, Buy, Limit, 2000, 50);
    f.take();
    f.book.modify(1, 2000, 60);
    expect_events(f, {mod(1, 2000, 60, false)});
    expect_queue(f, Buy, 2000, {2, 1});
    CHECK(f.book.volume_at(Buy, 2000) == 110);
}

TEST_CASE_TEMPLATE("modify: changing price loses queue priority", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Buy, Limit, 2000, 50);
    f.book.add(2, Buy, Limit, 1999, 50);
    f.take();
    f.book.modify(1, 1999, 50);
    expect_events(f, {mod(1, 1999, 50, false)});
    expect_queue(f, Buy, 1999, {2, 1});
    CHECK(f.book.volume_at(Buy, 2000) == 0);
    CHECK(f.book.best_bid() == 1999);
}

TEST_CASE_TEMPLATE("modify to a crossing price trades immediately", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Sell, Limit, 2005, 30);
    f.book.add(2, Buy,  Limit, 2000, 50);
    f.take();
    f.book.modify(2, 2005, 50);
    expect_events(f, {mod(2, 2005, 50, false), trade(2, 1, Buy, 2005, 30)});
    CHECK_FALSE(f.book.best_ask().has_value());
    CHECK(f.book.best_bid() == 2005);
    CHECK(f.book.volume_at(Buy, 2005) == 20);
}

TEST_CASE_TEMPLATE("modify rejects bad requests and changes nothing", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Buy, Limit, 2000, 50);
    f.take();
    f.book.modify(99, 2000, 10);
    f.book.modify(1, 2000, 0);
    f.book.modify(1, 5000, 10);
    expect_events(f, {rej(99, UnknownOrderId), rej(1, ZeroQuantity), rej(1, PriceOutOfBand)});
    CHECK(f.book.volume_at(Buy, 2000) == 50);
}

// =====================================================================
// Validation and capacity
// =====================================================================

TEST_CASE_TEMPLATE("validation rejects bad orders, in the documented order", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Buy,  Limit, 2000, 0);    // zero quantity
    f.book.add(2, Buy,  Limit, 1899, 10);   // just below the band
    f.book.add(3, Sell, Limit, 2101, 10);   // just above the band
    f.book.add(4, Buy,  Limit, 1900, 10);   // exactly on the edge: allowed
    f.book.add(5, Sell, Limit, 2100, 10);   // exactly on the edge: allowed
    f.book.add(4, Buy,  Limit, 1950, 10);   // ID 4 is resting
    f.book.add(6, Buy,  Limit, 5000, 0);    // two problems: zero qty is checked first
    expect_events(f, {rej(1, ZeroQuantity),
                      rej(2, PriceOutOfBand),
                      rej(3, PriceOutOfBand),
                      acc(4, Buy, Limit, 1900, 10),
                      acc(5, Sell, Limit, 2100, 10),
                      rej(4, DuplicateOrderId),
                      rej(6, ZeroQuantity)});
    CHECK(f.book.order_count() == 2);
}

TEST_CASE_TEMPLATE("an order ID can be reused once the order is gone", Book, BOOK_TYPES) {
    Fixture<Book> f;
    f.book.add(1, Buy, Limit, 2000, 10);
    f.book.cancel(1);
    f.book.add(1, Buy, Limit, 2000, 10);
    expect_events(f, {acc(1, Buy, Limit, 2000, 10),
                      cxl(1, 10, UserRequested),
                      acc(1, Buy, Limit, 2000, 10)});
    CHECK(f.book.order_count() == 1);
}

TEST_CASE_TEMPLATE("a full book rejects orders that could rest", Book, BOOK_TYPES) {
    Fixture<Book> f(/*capacity=*/2);
    f.book.add(1, Buy, Limit, 2000, 10);
    f.book.add(2, Buy, Limit, 1999, 10);
    f.take();

    f.book.add(3, Buy,  Limit,    1998, 10);   // could rest: rejected
    f.book.add(4, Sell, PostOnly, 2010, 10);   // could rest: rejected
    f.book.add(5, Sell, IOC,      2000, 5);    // never rests: allowed
    // Deliberate simplification: a limit order is rejected when the book
    // is full even if it would have traded completely.
    f.book.add(6, Sell, Limit,    2000, 5);
    expect_events(f, {rej(3, BookFull),
                      rej(4, BookFull),
                      acc(5, Sell, IOC, 2000, 5),
                      trade(5, 1, Sell, 2000, 5),
                      rej(6, BookFull)});

    f.book.cancel(2);                          // frees a slot
    f.book.add(3, Buy, Limit, 1998, 10);
    expect_events(f, {cxl(2, 10, UserRequested), acc(3, Buy, Limit, 1998, 10)});
}