// tests/check_naive_book.cpp
//
// Walks through the order book rules one scenario at a time,
// printing the events and the book after each step.

#include <cstdio>
#include <optional>

#include "lob/events.hpp"
#include "naive_book.hpp"

using namespace lob;

using Book = NaiveBook<RecordingListener>;

// Print recorded events, clear them, then print the book.
static void show(const char* title, const Book& book, RecordingListener& rec) {
    std::printf("\n>>> %s\n", title);
    for (const Event& e : rec.events) print_event(stdout, e);
    rec.clear();
    book.print(stdout);
}

static void print_best(const char* name, std::optional<Price> p) {
    if (p) std::printf("%s = %d\n", name, *p);
    else   std::printf("%s = none\n", name);
}

int main() {
    RecordingListener rec;
    Book book(rec, /*min_price=*/1900, /*max_price=*/2100, /*max_orders=*/1000);

    // 1. Build a book with resting limit orders.
    book.add(1, Side::Sell, OrderType::Limit, 2005, 100);
    book.add(2, Side::Sell, OrderType::Limit, 2005, 50);
    book.add(3, Side::Sell, OrderType::Limit, 2007, 70);
    book.add(4, Side::Buy,  OrderType::Limit, 2000, 80);
    book.add(5, Side::Buy,  OrderType::Limit, 2001, 40);
    show("1. Five limit orders rest (no prices cross)", book, rec);

    // 2. A buy at 2005 crosses the best ask. Time priority: #1 fills before #2.
    book.add(6, Side::Buy, OrderType::Limit, 2005, 120);
    show("2. Buy 120 @ 2005: fills #1 fully, then 20 of #2", book, rec);

    // 3. Market order sweeps every ask; what's left is cancelled.
    book.add(7, Side::Buy, OrderType::Market, 0, 200);
    show("3. Market buy 200: sweeps 2005 and 2007, leftover 100 cancelled", book, rec);

    // 4. Post-only: rejected if it would trade, rests otherwise.
    book.add(8, Side::Sell, OrderType::PostOnly, 2001, 10);
    book.add(9, Side::Sell, OrderType::PostOnly, 2003, 10);
    show("4. Post-only sells: @2001 would cross (rejected), @2003 rests", book, rec);

    // 5. Fill-or-kill: all or nothing.
    book.add(10, Side::Sell, OrderType::FOK, 2000, 200);
    book.add(11, Side::Sell, OrderType::FOK, 2000, 100);
    show("5. FOK sells: 200 can't fully fill (killed), 100 can (fills)", book, rec);

    // 6. Modify: reduce keeps priority, increase loses it.
    book.add(12, Side::Buy, OrderType::Limit, 2000, 30);
    book.add(13, Side::Buy, OrderType::Limit, 2000, 25);
    book.modify(12, 2000, 10);   // smaller, same price -> stays in place
    book.modify(4,  2000, 50);   // bigger -> moves to back of the queue
    show("6. Modify: #12 reduced (kept place), #4 increased (moved to back)", book, rec);

    // 7. Modify to a crossing price trades immediately.
    book.modify(13, 2003, 25);
    show("7. Modify #13 to 2003: crosses the ask, trades 10, rests 15", book, rec);

    // 8. Cancels and rejects.
    book.cancel(4);
    book.cancel(4);                                        // already gone
    book.add(12, Side::Buy, OrderType::Limit, 1990, 5);    // #12 still resting
    book.add(14, Side::Buy, OrderType::Limit, 5000, 10);   // outside 1900..2100
    book.add(15, Side::Buy, OrderType::Limit, 2000, 0);    // zero quantity
    show("8. Cancel #4, cancel again, then three invalid orders", book, rec);

    std::printf("\n");
    print_best("best bid", book.best_bid());
    print_best("best ask", book.best_ask());
    std::printf("resting orders = %zu\n", book.order_count());

    // 9. A tiny book with room for only 2 orders.
    RecordingListener rec2;
    Book small(rec2, 1900, 2100, /*max_orders=*/2);
    small.add(1, Side::Buy, OrderType::Limit, 2000, 10);
    small.add(2, Side::Buy, OrderType::Limit, 2000, 10);
    small.add(3, Side::Buy, OrderType::Limit, 2000, 10);
    show("9. Capacity 2: third order rejected", small, rec2);
    return 0;
}