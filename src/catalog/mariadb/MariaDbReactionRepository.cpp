#include "caelitus/catalog/mariadb/MariaDbReactionRepository.hpp"

#include "catalog/mariadb/SqlFilter.hpp"

#include <algorithm>
#include <map>
#include <unordered_set>

namespace caelitus::catalog::mariadb {

using db::Row;

namespace {

constexpr std::size_t kChunk = 500;  // rows per multi-row statement

RankedBook toRanked(const Row& r) {
    return {BookId(r.get<std::int64_t>("id")),
            r.get<std::string>("title"),
            {r.get<std::int64_t>("likes"), r.get<std::int64_t>("dislikes")}};
}

}  // namespace

std::int64_t MariaDbReactionRepository::add(const std::vector<DailyReactions>& deltas) {
    if (deltas.empty()) return 0;

    // Keep only books that exist (a deleted book's likes are simply dropped).
    std::vector<BookId> ids;
    for (const auto& d : deltas) ids.push_back(d.book);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    std::unordered_set<std::int64_t> existing;
    for (std::size_t i = 0; i < ids.size(); i += kChunk) {
        const std::vector<BookId> chunk(ids.begin() + static_cast<std::ptrdiff_t>(i),
                                        ids.begin() + static_cast<std::ptrdiff_t>(std::min(ids.size(), i + kChunk)));
        for (const auto& r :
             sql_->query("SELECT id FROM books WHERE id IN (" + placeholders(chunk.size()) + ")", idParams(chunk)))
            existing.insert(r.get<std::int64_t>("id"));
    }

    std::int64_t skipped = 0;
    std::vector<const DailyReactions*> kept;
    std::map<std::int64_t, ReactionCounts> totals;  // ordered: stable lock order
    for (const auto& d : deltas) {
        if (!existing.count(d.book.value)) {
            skipped += d.likes + d.dislikes;
            continue;
        }
        kept.push_back(&d);
        totals[d.book.value].likes += d.likes;
        totals[d.book.value].dislikes += d.dislikes;
    }

    for (std::size_t i = 0; i < kept.size(); i += kChunk) {
        std::string values;
        db::Params params;
        for (std::size_t k = i; k < std::min(kept.size(), i + kChunk); ++k) {
            values += (k == i ? "(?, ?, ?, ?)" : ", (?, ?, ?, ?)");
            params.insert(params.end(), {kept[k]->book.value, kept[k]->day, kept[k]->likes, kept[k]->dislikes});
        }
        sql_->execute(
            "INSERT INTO book_reactions_daily (book_id, day, likes, dislikes) VALUES " + values +
                " ON DUPLICATE KEY UPDATE likes = likes + VALUES(likes), dislikes = dislikes + VALUES(dislikes)",
            params);
    }

    // One UPDATE per chunk of books: likes = likes + CASE id WHEN ? THEN ? ... END
    std::vector<std::pair<std::int64_t, ReactionCounts>> books(totals.begin(), totals.end());
    for (std::size_t i = 0; i < books.size(); i += kChunk) {
        const std::size_t end = std::min(books.size(), i + kChunk);
        std::string likeCase, dislikeCase;
        db::Params likeParams, dislikeParams, idList;
        for (std::size_t k = i; k < end; ++k) {
            likeCase += " WHEN ? THEN ?";
            dislikeCase += " WHEN ? THEN ?";
            likeParams.insert(likeParams.end(), {books[k].first, books[k].second.likes});
            dislikeParams.insert(dislikeParams.end(), {books[k].first, books[k].second.dislikes});
            idList.emplace_back(books[k].first);
        }
        db::Params params = likeParams;
        params.insert(params.end(), dislikeParams.begin(), dislikeParams.end());
        params.insert(params.end(), idList.begin(), idList.end());
        sql_->execute("UPDATE books SET likes = likes + CASE id" + likeCase +
                          " ELSE 0 END,"
                          " dislikes = dislikes + CASE id" +
                          dislikeCase +
                          " ELSE 0 END"
                          " WHERE id IN (" +
                          placeholders(end - i) + ")",
                      params);
    }
    return skipped;
}

std::vector<ReactionCounts> MariaDbReactionRepository::counts(BookId book,
                                                              const std::vector<std::optional<DateRange>>& ranges) {
    std::vector<ReactionCounts> out(ranges.size());

    std::string sums;
    db::Params params;
    std::optional<Date> earliest;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (!ranges[i]) continue;
        const std::string n = std::to_string(i);
        sums += (sums.empty() ? "" : ", ") + std::string("SUM(IF(day BETWEEN ? AND ?, likes, 0)) AS l") + n +
                ", SUM(IF(day BETWEEN ? AND ?, dislikes, 0)) AS d" + n;
        params.insert(params.end(), {ranges[i]->from, ranges[i]->to, ranges[i]->from, ranges[i]->to});
        if (!earliest || ranges[i]->from < *earliest) earliest = ranges[i]->from;
    }

    if (!sums.empty()) {
        params.insert(params.end(), {book.value, *earliest});
        auto row =
            sql_->queryOne("SELECT " + sums + " FROM book_reactions_daily WHERE book_id = ? AND day >= ?", params);
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            if (!ranges[i] || !row) continue;
            const std::string n = std::to_string(i);
            out[i] = {row->getOptional<std::int64_t>("l" + n).value_or(0),
                      row->getOptional<std::int64_t>("d" + n).value_or(0)};
        }
    }

    if (std::any_of(ranges.begin(), ranges.end(), [](const auto& r) { return !r.has_value(); })) {
        auto row = sql_->queryOne("SELECT likes, dislikes FROM books WHERE id = ?", {book.value});
        const ReactionCounts all =
            row ? ReactionCounts{row->get<std::int64_t>("likes"), row->get<std::int64_t>("dislikes")}
                : ReactionCounts{};
        for (std::size_t i = 0; i < ranges.size(); ++i)
            if (!ranges[i]) out[i] = all;
    }
    return out;
}

std::vector<RankedBook> MariaDbReactionRepository::top(const std::optional<DateRange>& range, ReactionOrder order,
                                                       int limit) {
    const bool liked = order == ReactionOrder::MostLiked;
    if (!range) {
        // All time: the indexed generated column reaction_score = likes - dislikes.
        return sql_->queryList(std::string("SELECT id, title, likes, dislikes FROM books WHERE ") +
                                   (liked ? "likes > 0 ORDER BY reaction_score DESC, likes DESC, id"
                                          : "dislikes > 0 ORDER BY reaction_score ASC, dislikes DESC, id") +
                                   " LIMIT ?",
                               {limit}, toRanked);
    }
    return sql_->queryList(
        std::string("SELECT r.book_id AS id, b.title, SUM(r.likes) AS likes, SUM(r.dislikes) AS dislikes"
                    " FROM book_reactions_daily r JOIN books b ON b.id = r.book_id"
                    " WHERE r.day BETWEEN ? AND ? GROUP BY r.book_id, b.title") +
            (liked
                 ? " HAVING SUM(r.likes) > 0 ORDER BY SUM(r.likes) - SUM(r.dislikes) DESC, SUM(r.likes) DESC, r.book_id"
                 : " HAVING SUM(r.dislikes) > 0 ORDER BY SUM(r.likes) - SUM(r.dislikes) ASC, SUM(r.dislikes) DESC,"
                   " r.book_id") +
            " LIMIT ?",
        {range->from, range->to, limit}, toRanked);
}

std::int64_t MariaDbReactionRepository::deleteBefore(const Date& day) {
    // Uses the index on `day`; deletes in chunks so one run never holds locks for long.
    std::int64_t total = 0;
    while (true) {
        const auto n = static_cast<std::int64_t>(
            sql_->execute("DELETE FROM book_reactions_daily WHERE day < ? LIMIT 10000", {day}).affectedRows);
        total += n;
        if (n < 10000) return total;
    }
}

}  // namespace caelitus::catalog::mariadb
