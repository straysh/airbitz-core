/*
 *  Copyright (c) 2014, AirBitz, Inc.
 *  All rights reserved.
 */

#include "TxUpdater.hpp"
#include "../util/Debug.hpp"
#include <list>

namespace abcd {

constexpr unsigned max_queries = 10;

/**
 * An address that needs to be checked.
 * More outdated addresses sort earlier in the list.
 */
struct ToCheck
{
    bc::client::sleep_time oldness;
    bc::payment_address address;

    bool
    operator<(const ToCheck &b) const
    {
        return oldness > b.oldness;
    }
};

using std::placeholders::_1;

TxUpdater::~TxUpdater()
{
}

TxUpdater::TxUpdater(TxDatabase &db, bc::client::obelisk_codec &codec,
    TxCallbacks &callbacks):
    db_(db), codec_(codec),
    callbacks_(callbacks),
    failed_(false),
    queued_queries_(0),
    queued_get_indices_(0),
    last_wakeup_(std::chrono::steady_clock::now())
{
}

void TxUpdater::start()
{
    // Check for new blocks:
    get_height();

    // Handle block-fork checks & unconfirmed transactions:
    db_.foreach_unconfirmed(std::bind(&TxUpdater::get_index, this, _1));
    queue_get_indices();

    // Transmit all unsent transactions:
    db_.foreach_unsent(std::bind(&TxUpdater::send_tx, this, _1));
}

void TxUpdater::watch(const bc::payment_address &address,
    bc::client::sleep_time poll)
{
    // Only insert if it isn't already present:
    rows_[address] = AddressRow{poll, std::chrono::steady_clock::now() - poll};
    if (queued_queries_ < max_queries)
        query_address(address);
}

void TxUpdater::send(bc::transaction_type tx)
{
    if (db_.insert(tx, TxState::unsent))
        callbacks_.on_add(tx);
    send_tx(tx);
}

AddressSet TxUpdater::watching()
{
    AddressSet out;
    for (auto &row: rows_)
        out.insert(row.first);
    return out;
}

bc::client::sleep_time TxUpdater::wakeup()
{
    bc::client::sleep_time next_wakeup(0);
    auto now = std::chrono::steady_clock::now();

    // Figure out when our next block check is:
    auto period = std::chrono::seconds(30);
    auto elapsed = std::chrono::duration_cast<bc::client::sleep_time>(
        now - last_wakeup_);
    if (period <= elapsed)
    {
        get_height();
        last_wakeup_ = now;
        elapsed = bc::client::sleep_time::zero();
    }
    next_wakeup = period - elapsed;

    // Build a list of all the addresses that are due for a checkup:
    std::list<ToCheck> toCheck;
    for (auto &row: rows_)
    {
        auto poll_time = row.second.poll_time;
        auto elapsed = std::chrono::duration_cast<bc::client::sleep_time>(
            now - row.second.last_check);
        if (poll_time <= elapsed)
            toCheck.push_back(ToCheck{elapsed - poll_time, row.first});
        else
            next_wakeup = bc::client::min_sleep(next_wakeup, poll_time - elapsed);
    }

    // Process the most outdated addresses first:
    toCheck.sort();
    for (const auto &i: toCheck)
    {
        auto &row = rows_[i.address];
        if (queued_queries_ < max_queries ||
            row.poll_time < std::chrono::seconds(2))
        {
            next_wakeup = bc::client::min_sleep(next_wakeup, row.poll_time);
            query_address(i.address);
        }
    }

    // Report the last server failure:
    if (failed_)
    {
        callbacks_.on_fail();
        failed_ = false;
    }

    return next_wakeup;
}

void TxUpdater::watch_tx(bc::hash_digest txid, bool want_inputs)
{
    db_.reset_timestamp(txid);
    if (!db_.txidExists(txid))
        get_tx(txid, want_inputs);
    else if (want_inputs)
        get_inputs(db_.txidLookup(txid));
}

void TxUpdater::get_inputs(const bc::transaction_type &tx)
{
    for (auto &input: tx.inputs)
        watch_tx(input.previous_output.hash, false);
}

void TxUpdater::query_done()
{
    --queued_queries_;
    if (!queued_queries_)
        callbacks_.on_quiet();
}

void TxUpdater::queue_get_indices()
{
    if (queued_get_indices_)
        return;
    db_.foreach_forked(std::bind(&TxUpdater::get_index, this, _1));
}

// - server queries --------------------

void TxUpdater::get_height()
{
    auto on_error = [this](const std::error_code &error)
    {
        if (!failed_)
            ABC_DebugLog("fetch_last_height failed: %s",
                error.message().c_str());
        failed_ = true;
    };

    auto on_done = [this](size_t height)
    {
        if (height != db_.last_height())
        {
            db_.at_height(height);
            callbacks_.on_height(height);

            // Query all unconfirmed transactions:
            db_.foreach_unconfirmed(std::bind(&TxUpdater::get_index, this, _1));
            queue_get_indices();
        }
    };

    codec_.fetch_last_height(on_error, on_done);
}

void TxUpdater::get_tx(bc::hash_digest txid, bool want_inputs)
{
    ++queued_queries_;

    auto on_error = [this, txid, want_inputs](const std::error_code &error)
    {
        // A failure means the transaction might be in the mempool:
        (void)error;
        get_tx_mem(txid, want_inputs);
        query_done();
    };

    auto on_done = [this, txid, want_inputs](const bc::transaction_type &tx)
    {
        BITCOIN_ASSERT(txid == bc::hash_transaction(tx));
        if (db_.insert(tx, TxState::unconfirmed))
            callbacks_.on_add(tx);
        if (want_inputs)
            get_inputs(tx);
        get_index(txid);
        query_done();
    };

    codec_.fetch_transaction(on_error, on_done, txid);
}

void TxUpdater::get_tx_mem(bc::hash_digest txid, bool want_inputs)
{
    ++queued_queries_;

    auto on_error = [this](const std::error_code &error)
    {
        if (!failed_)
            ABC_DebugLog("fetch_unconfirmed_transaction failed: %s",
                error.message().c_str());
        failed_ = true;
        query_done();
    };

    auto on_done = [this, txid, want_inputs](const bc::transaction_type &tx)
    {
        BITCOIN_ASSERT(txid == bc::hash_transaction(tx));
        if (db_.insert(tx, TxState::unconfirmed))
            callbacks_.on_add(tx);
        if (want_inputs)
            get_inputs(tx);
        get_index(txid);
        query_done();
    };

    codec_.fetch_unconfirmed_transaction(on_error, on_done, txid);
}

void TxUpdater::get_index(bc::hash_digest txid)
{
    ++queued_get_indices_;

    auto on_error = [this, txid](const std::error_code &error)
    {
        // A failure means that the transaction is unconfirmed:
        (void)error;
        db_.unconfirmed(txid);

        --queued_get_indices_;
        queue_get_indices();
    };

    auto on_done = [this, txid](size_t block_height, size_t index)
    {
        // The transaction is confirmed:
        (void)index;

        db_.confirmed(txid, block_height);

        --queued_get_indices_;
        queue_get_indices();
    };

    codec_.fetch_transaction_index(on_error, on_done, txid);
}

void TxUpdater::send_tx(const bc::transaction_type &tx)
{
    auto on_error = [this, tx](const std::error_code &error)
    {
        //server_fail(error);
        db_.forget(bc::hash_transaction(tx));
        callbacks_.on_send(error, tx);
    };

    auto on_done = [this, tx]()
    {
        std::error_code error;
        db_.unconfirmed(bc::hash_transaction(tx));
        callbacks_.on_send(error, tx);
    };

    codec_.broadcast_transaction(on_error, on_done, tx);
}

void TxUpdater::query_address(const bc::payment_address &address)
{
    ++queued_queries_;
    rows_[address].last_check = std::chrono::steady_clock::now();

    auto on_error = [this](const std::error_code &error)
    {
        if (!failed_)
            ABC_DebugLog("address_fetch_history failed: %s",
                error.message().c_str());
        failed_ = true;
        query_done();
    };

    auto on_done = [this](const bc::client::history_list &history)
    {
        for (auto &row: history)
        {
            watch_tx(row.output.hash, true);
            if (row.spend.hash != bc::null_hash)
                watch_tx(row.spend.hash, true);
        }
        query_done();
    };

    codec_.address_fetch_history(on_error, on_done, address);
}

} // namespace abcd
