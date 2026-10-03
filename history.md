# How caelitus was built

A record of the conversations in which this project was designed and written:
what was asked, what was decided and why, what was built, and what went wrong
along the way. It was written in pair-programming sessions between the project
owner (who set goals and made the decisions) and an AI assistant (Claude, in
Claude Code), in Greek; this is an English summary. Times are Athens time.

- **Session 1:** Friday 2 October 2026, 21:31 – 01:41 (the database layer up to a working web UI).
- **Session 2:** Saturday 3 October 2026, from 01:57 (restructuring, documentation, sample data, Docker).

The way of working, set by the owner on the first message and kept throughout:
**"σιγά σιγά"**, step by step. Each module is designed first, the open
questions are put to the owner, then it is built, tested (with sanitizers, and
against a real MariaDB and MQTT broker), and only then the next step is
proposed.

---

## Session 1: from an empty project to a running system

### 1. The goal

The owner's opening message: a C++17 project that will eventually have JSON,
MQTT, an in-memory cache, MariaDB (through the C++ connector), exceptions and a
TCP server, built gradually. The first milestone: *"a solid foundation for the
database: the classes services and repositories need to reach the database
without errors, without the services knowing which database it is or what it
does."*

### 2. The database layer

Built: the layering that still exists today.

```text
Service            → knows only repository interfaces + ITransactionManager
Repository (impl)  → knows SQL, uses SqlExecutor
SqlExecutor / TransactionManager → ConnectionPool → IConnection
                                                     └─ mariadb/MariaDbConnection
```

- `DbErrors.hpp`: a database-agnostic exception hierarchy with `isTransient()`.
- `DbValue`: statement parameters; `"abc"` binds as a string, not as `bool` (a
  classic C++17 trap); `std::optional` binds as NULL.
- `Row`: rows fully read into memory with typed, checked access (`"9.5"` read as
  `int`, or `300` as `uint8_t`, throws instead of silently truncating).
- `ConnectionPool`: bounded, thread-safe, pings idle connections, drops broken
  ones, rolls back connections returned with an open transaction; `warmUp()` so
  wrong credentials fail at startup.
- `SqlExecutor` and `ITransactionManager` / `TransactionManager` with deadlock
  retries and nested calls joining the outer transaction.

Edge cases handled on purpose from day one: a connection lost during COMMIT is
never retried (the outcome is unknown); an error swallowed inside a transaction
still prevents the commit; a failed SELECT outside a transaction is retried once
on a fresh connection, a write never is. The rule that follows, stated then and
still true: *the code inside `inTransaction()` may run more than once, so it
must not publish MQTT messages or do anything else outside the database.*

Tests: 31 unit tests with a fake connection, 9 integration tests against a real
MariaDB 11 in a throwaway container, all under AddressSanitizer and UBSan. A
small in-house test harness was written because GoogleTest was not installed.
`cmake_minimum_required` was lowered from 4.3 to 3.20 to match the system CMake.

### 3. Isolation levels and logging

Asked for by the owner ("spdlog is perhaps a good choice, but whatever you
think best").

- Per-transaction isolation (`SET TRANSACTION ISOLATION LEVEL …` before
  `START TRANSACTION [READ ONLY]`), verified on a real server: READ COMMITTED
  sees another thread's commit, REPEATABLE READ keeps its snapshot, read-only
  rejects writes. A nested call asking for a different level throws instead of
  being silently ignored.
- A project-wide logging module on spdlog: named, hierarchical loggers
  (`db.sql`, `db.tx`, `db.pool`, …), per-logger levels, console and rotating
  file; an unknown level name is an error (spdlog alone would silently turn it
  into "off").
- The logging principle: *log what the caller does not see; what reaches the
  caller as an exception stays at debug*, so nothing is reported twice. SQL is
  logged without parameter values, so passwords and personal data never reach a
  log (a test checks it).

The owner then installed `libspdlog-dev`, and the build switched from
downloading spdlog to the system package.

### 4. Dates and times

The owner: no BLOBs needed, dates yes.

- `Timestamp` (UTC, **microseconds**: the precision MariaDB stores, and unlike
  nanoseconds it reaches beyond the year 2262, so `'9999-12-31'` fits) and
  `Date` (always valid).
- The database session is forced to UTC; an integration test checks that the
  server's `NOW(6)` agrees with our clock.
- Lossy conversions are refused (a DATETIME cannot be read as a Date); MariaDB
  zero dates throw. TIME columns were left out (they are durations in MariaDB,
  not times of day).
- Calendar arithmetic uses Howard Hinnant's public-domain algorithms, tested for
  every day from year 0 to 9999.

### 5. JSON and configuration from a file

The owner specified a central `AppConfig` class with nested sections matching a
JSON sample (`mqtt`, `db`, `server`), read from `config/config.json`.

- nlohmann/json, with `Timestamp` ↔ RFC 3339 and `Date` ↔ `"YYYY-MM-DD"`.
- Strict loading: unknown keys are errors (a typo never goes unnoticed), types
  and ranges are checked, error messages carry the full path
  (`config/config.json: db.pool.maxSize: must be between 1 and 1000`).
- `"${NAME}"` values come from environment variables, so the database password
  never sits in a file. Comments are allowed. Durations carry their unit in the
  key name.
- File lookup: `--config`, `$CAELITUS_CONFIG`, `./config/config.json`, then next
  to the executable (so CLion's `cmake-build-debug/` works).
- Open question answered by interpretation: `server.log` in the sample was taken
  as the level of the TCP server's logger; application-wide logging got its own
  `log` section.

### 6. MQTT, behind an abstraction

The owner's requirements: mosquitto for now, but possibly something else later,
so services must not know which library is used; plain text messages; QoS
configurable (usually 0), defaults for retain and sessions; easy publishing from
services; receiving messages on subscribed topics; clean logging *without
flooding*.

```text
Service ──► IMqttPublisher / IMqttSubscriber
                 ▲
          MqttClientBase     library-independent: subscriptions, dispatch, logging
                 ▲
          MosquittoClient    only connect / publish / subscribe
```

- Publishing never fails a service: without a connection it returns false and
  the message is dropped (logged); exceptions only for misuse (a wildcard in a
  topic).
- Subscriptions can be made before connecting and are restored on every
  reconnect (verified by restarting the broker in a test).
- Handlers run on a dispatcher thread, so a slow handler cannot block the
  network thread.
- `LogThrottle`: a repeating warning appears at most once a minute, with the
  count of what was suppressed; 100 publishes without a connection produce one
  line (tested). A broker restart logs exactly three lines.

### 7. Last will

The owner asked for it. Since a broker sends the will only when a connection
breaks, not on a normal disconnect, the usual pattern was implemented: the
retained will says `offline`; the client publishes `online` on every connect and
`offline` itself before a clean stop. The crash case was tested with a child
process that exits without stopping the client; the broker published `offline`
on its own.

### 8. The TCP server, and the question of an API specification

The owner: web clients will connect; "it could be REST, but for some reason it
is pure JSON over TCP"; request/response; messages usually end with `\0`; **no
thread per connection**; it should be able to become very fast. And a question:
*"it is not HTTP/REST, it is plain JSON, but how could I have a nice
specification? Normally we would have Swagger."* The owner also introduced the
use case that would drive everything after: a **book catalog** with authors,
books, tags, reviews, categories, and filters by date and category.

Built: an Asio server with 2 I/O threads and 8 workers; complete messages go to
workers, so a slow query never freezes other connections (tested: a fast request
answered in under 300 ms while another ran for 800 ms). Limits on connections and
message size, backpressure, idle timeout, graceful shutdown. 20 tests on real
sockets, 0 data races under ThreadSanitizer, about 84,000 requests/s in a release
build with 50 clients.

The proposal for the specification: **JSON-RPC 2.0** as the message envelope and
**OpenRPC** ("Swagger for JSON-RPC") as the specification, generated
**code-first** from a single declaration per method that also drives request
validation. The owner liked it ("εξαιρετική ιδέα") but deliberately postponed it
"towards the end".

### 9. The book catalog: design first

The owner asked to design the service/repository structure before writing code.
The design: `domain/` (entities, typed ids, repository interfaces), `service/`
(use cases, rules, transactions, events), `infra/` (SQL), with dependencies
pointing inward. The owner's decisions:

| Question | Decision |
|---|---|
| Categories | **Flat**, one per book |
| Reviews | Just a reviewer **name**; there is no concept of a user |
| Tags | Free-form, created on first use |
| Deletes | Hard deletes; refused for authors and categories that still have books |
| Concurrent edits | Optimistic locking (`version`) on books and authors |
| Paging | `page` / `pageSize` with a total |
| Text search | "Title contains" now, full-text later |

Consequence noted at the time: without users, "one review per reader" cannot be
enforced, so it is not.

Built: migrations (`schema_migrations`, checksums), the domain, the MariaDB
repositories (search with a parameter-only WHERE builder, a whitelisted ORDER BY,
no N+1 queries), and the services. Bug found by a test: two concurrent updates
of the same review could read the same old rating; fixed with
`SELECT … FOR UPDATE` (`lockById`), and confirmed by removing the lock (the test
failed 3 of 3 times) and restoring it.

### 10. Likes and dislikes over MQTT

The owner's idea: likes and dislikes arrive as MQTT messages; results are read
through JSON requests, including "most liked today, yesterday, in the last year,
ever".

Design proposed: per-day totals (`book_reactions_daily`) instead of a row per
like, all-time totals on the book row, and an in-memory buffer flushed once a
second (one transaction per second instead of one per like). The owner asked for
a JSON-RPC router covering **all** services, not just reactions, left OpenRPC for
the end, and left the rest to the assistant's proposals: count events without
identity, topics `catalog/in/books/{id}/like|dislike` (kept apart from the
outgoing events), days in Europe/Athens time, ranking by likes − dislikes,
rolling periods.

Mid-way the owner stopped to make sure of the model: *"we subscribe once per
book?"* The answer: two wildcard subscriptions (`catalog/in/books/+/like`)
cover every book. The owner then improved the design: *a book should have a field
saying whether it takes likes; we subscribe to all, ignore messages for books
without the flag, and the check goes through a cache of the books: "εδώ μπαίνει
η cache".* Decisions: the flag is off by default, the whole cache is loaded at
startup, a single server instance, and a dedicated `books.setReactionsEnabled`.

Built: `TimeZone` with the EU daylight-saving rule (C++17 has no time zones),
`PeriodicTask`, a generic `LocalCache` and the `BookCache`, migration 003,
`ReactionService`, `MqttReactionListener`, and the JSON-RPC router with all 26
methods. Three real bugs were found by the tests:

1. **A multi-row INSERT that never finished.** The driver computed generated ids
   for every row of large inserts; a 1,200-row flush hung. Fixed by separating
   `insert()` (returns the id) from `execute()`. This was the integration test
   that "hung" earlier.
2. **Writer starvation in the cache.** glibc's `std::shared_mutex` prefers
   readers, so under a storm of likes an update could wait forever; replaced by
   a writer-preferring lock.
3. **A lifetime bug in the MQTT listener**: a queued message could reach a
   destroyed listener; fixed, and the shutdown order changed so no like is lost
   on stop.

Verified end to end: a book enabled, three `mosquitto_pub` likes, and
`reactions.get` showed them 1.5 s later.

### 11. OpenRPC

Done as designed in step 8: every method declared once in C++ with JSON Schemas
for its parameters and result. From that declaration, requests are validated,
`rpc.discover` returns the specification, and `caelitus --openrpc` writes
`docs/openrpc.json`. The document was validated against the official OpenRPC
1.3.2 meta-schema (0 errors). Two tests stop the specification from lying: one
calls every method and checks the real result against its schema, the other
fails if `docs/openrpc.json` is behind the code. Both were confirmed to catch a
deliberately renamed field.

### 12. A web UI

The owner asked whether a Node.js web UI could be built alongside. The
constraint explained first: a browser cannot open a TCP socket, so a Node
**gateway** sits in between, forwarding the same JSON-RPC over HTTP, pushing live
MQTT traffic over a WebSocket, and turning like buttons into MQTT messages (the
same path a device would use). Order agreed: OpenRPC first, the UI on top of it.
The owner's choices: for both administrators and readers, React + Vite +
TypeScript, no login, a monorepo (`web/`). The owner installed Node 22 with nvm,
and asked for the UI to show all the services and APIs, book search, a **live
dashboard** of likes and events, and 100-150 books in the database.

Built: the gateway (Fastify), three pages (Books, Live, API with a "try it"
form), TypeScript types generated from `docs/openrpc.json`, a seed of 144 real
books loaded through the API, and a like simulator. Problems fixed after looking
at screenshots: the `/live` page collided with the WebSocket path; the chart
showed zeros for the time before the page was opened and a false drop at the end;
the filters overflowed on mobile.

### 13. Trying it live

The owner asked to try it before writing more tests. Everything was started
locally (database, server, gateway, UI), the simulator ran at 20 and then 100
likes/s (the server kept up, storing ~90/s with a sanitizer build), and the
server was stopped to watch the shutdown: it waited for the gateway's
connections, unsubscribed from likes **before** the final flush, stored the 58
likes still in its buffer, and published `offline`; the UI showed "Server
offline". The owner: *"η δουλειά που έχει γίνει είναι εκπληκτική."*

One observation recorded then: while the server is down the dashboard chart
still shows likes, because it counts what was *sent* over MQTT, not what was
*counted*.

### 14. The request that started session 2

> "For the C++ server, split the code better: we have `src`, add an `include`
> directory. Comment everything so that documentation can be generated, with
> Doxygen or something more modern. Then a `history.md` with everything we wrote
> here. Then a perfect, technical `README.md` that explains everything about the
> server as if to a junior developer. The UI is fine; perhaps a more professional
> look, only if you feel like it. For the UI too I want everything: what it does,
> how to run it, Docker, simulations. For the database, a script with the schema
> and a few hundred books. Put your best into the C++; do any refactoring you
> want."

The plan accepted: Doxygen with the doxygen-awesome theme (the "more modern"
tools either sit on Doxygen themselves or are less mature). The owner installed
`doxygen graphviz clang-format libasio-dev nlohmann-json3-dev`. The restructuring
had started (public headers moved to `include/caelitus/`) when the session ended
abruptly: the laptop shut down.

---

## Session 2: restructuring, documentation, data, Docker

### 15. Recovering

The owner: *"I don't know what happened, but what we were doing was
interrupted."* The state was reconstructed from the previous transcript: the
last action had been the first build after the restructure. It failed on a
missing `#include`; once fixed, everything built and the unit tests passed. The
task list was then written to the assistant's persistent memory, so another
abrupt shutdown would not lose it, and the owner confirmed the order: C++ first,
then the books, README, history, the UI documentation and, optionally, the UI's
look.

### 16. A real bug: the server could stay "online" after stopping

Running the integration tests against the real broker, one test failed every
time. The cause was in the client, not the test: on connect, the network thread
marked the client connected *before* publishing `online`; a `stop()` arriving in
between published `offline` first, and the late `online` overwrote it. Both are
retained, so the broker would report a stopped server as online forever. Fixed by
ordering the two publishes under one lock and never sending `online` after
`offline`. A deterministic unit test with the fake transport now covers it
(confirmed to fail without the fix). Eleven stale retained test messages were
cleaned from the broker.

### 17. The C++ restructure

- **Public headers** in `include/caelitus/<module>/`, implementation and private
  headers in `src/<module>/`; `main.cpp` split into a thin `main()` and an
  `Application` class (the composition root); `--help` and `--version`; CMake
  presets (`asan`, `debug`, `tsan`, `release`) that CLion picks up.
- **Driver code in separate libraries**: `db` / `db_mariadb`, `mqtt` /
  `mqtt_mosquitto`, `catalog` / `catalog_mariadb` (the old `infra`, renamed after
  the technology it contains). The catalog services now cannot even *link*
  against MariaDB or libmosquitto, which turns the founding rule of step 1 into
  something the build enforces (checked: the catalog library contains no driver
  symbols).
- **`.clang-format`**, tuned to the existing style and applied to every file.
- **Stricter warnings** (`-Wshadow -Wold-style-cast -Wsign-conversion …`) with
  zero warnings on GCC and clang; clang found one real portability issue (a
  lambda capturing a structured binding, which is C++20).
- **ThreadSanitizer** found a use-after-free in the MQTT integration tests (an
  inbox destroyed before the client delivering to it); fixed by giving the
  handler shared ownership of the inbox.
- **A regression the restructure introduced**, found while documenting: the
  executable moved to `build/<preset>/src/`, too deep for "next to the executable
  or one level up". The configuration is now searched in every directory above
  the executable, with a test.
- The API's version string now comes from the build instead of a literal.

### 18. Documentation

Every public header was documented with `///` comments (parameters, return
values, exceptions, thread safety, which logger), checked with a script that
proved only comments changed, never code. The Doxygen site has the
doxygen-awesome theme with dark mode, a page per module, an architecture
diagram, class diagrams, and **zero warnings** (from 789 at the start). Writing
the documentation surfaced two wrong statements, both corrected: the module page
first described the TCP framing as newline-based (it is `\0`) and the cache as
having expiry (it has none).

### 19. A few hundred books

`db/sample/catalog.json` became the single source of sample data: 528 real books
with their first publication year, 328 authors, 14 categories (three new:
popular science, history, comics), Greek and English titles. From it:
`npm run seed` (through the API) and `db/sample/catalog.sql` (schema plus data for
an empty database), generated by `db/sample/build.sh`, which loads the JSON into
a throwaway database through the real server and dumps the result. The like
history in the script is generated at load time relative to that day, so "today"
and "last 7 days" are never empty however old the file is. A test runs every
sample book through the service rules.

### 20. Docker

Dockerfiles for the server (building MariaDB Connector/C++ from source, since
Ubuntu does not package it) and for the web, and `docker/compose.yml` that starts
the whole system (MariaDB with the sample catalog, mosquitto, server, web) on
ports that do not clash with the owner's local MariaDB and broker. Verified: 528
books served, and a like sent through the gateway counted by the server.

### 21. README

`README.md`, written for a developer new to the code: quick start, building,
running, the API with every method and error, MQTT topics, architecture
(diagrams of the modules, a request and a like), each module explained, the
database, a configuration reference, logging, testing, Doxygen, step-by-step
recipes for common changes, conventions, troubleshooting, and the known limits.
Log lines, messages and examples in it were checked against the running server.

---

### 22. Documentation for the web side

`web/README.md` rewritten: what each page does, how the gateway fits between
the browser, the server and the broker, running in development, production and
Docker, the sample data and the simulator, every gateway endpoint and WebSocket
message, the UI's code layout and generated types, settings and
troubleshooting. Its claims were checked too, for example that a misspelled API
parameter in the UI code is a TypeScript compile error.

### 23. A more professional look

The owner had left this optional ("only if you feel like it"). Screenshots of
the UI with 528 books showed what to fix: a cloud of 387 tags taking half the
filter area, filters spread across the page above the results, emoji for likes,
and empty-looking dashboard panels. Changed:

- Books: a filter sidebar (collapsible on phones), the 18 most used tags plus a
  search over all of them, removable chips for the filters in effect, cards with
  a placeholder cover (initials, colored by category), star ratings and icons.
- The book panel: a header with the cover, likes per period as small cards, a
  star picker for reviews.
- A new set of design tokens (neutrals, shadows, radii), the Inter font bundled
  with the app, SVG icons instead of emoji, consistent empty and loading states.
  The chart colors were re-validated for color-blind readers and contrast on the
  new surfaces in both themes.
- **The open point from step 13**: when the server is offline, the Live page
  now says so in a banner, explains that the likes on the chart are sent but not
  counted, and the ranking reports itself unavailable instead of showing "no
  likes".

Verified with screenshots (desktop light/dark, phone, server stopped) and the
production build, also inside the Docker image.

---

## Session 2, continued: periodic jobs

### 24. A scheduler, a health check

The owner opened two new areas, periodic work ("every X seconds do something")
and a command-line tool, and asked for proposals. The proposal: a reusable
scheduler module first, then the CLI (a `caelitus` with subcommands for
operators, plus a `caelitusctl` client whose commands are generated from the
OpenRPC description). The owner's decisions: as proposed, starting with the
scheduler; job settings in `config.json`; JSON-RPC methods over TCP to stop and
start jobs; a **health** job every 15 seconds checking the database, the cache's
size, statistics and uptime, writing to the log when something is wrong; the
scheduler reusable inside the application so more jobs can be added later;
comments everywhere and every Markdown file updated.

Built:

- **`scheduler` module**, independent of the catalog: schedules `every 15s`
  (after the previous run ends), `rate 1s` (fixed rate, missed runs skipped) and
  `cron 0 3 * * *` with a cron evaluator that works in local time across the
  summer-time changes (`TimeZone` gained local-to-UTC conversion for it).
  One timer thread, a small worker pool, no job ever overlapping itself,
  timeouts reported, retries with a doubling delay, pause/resume/run-now, and a
  status per job. `PeriodicTask` was retired.
- **Five jobs**, defined in one place (`Application::defineJobs`):
  `reaction-flush`, `book-cache-reload` (both moved from `PeriodicTask`),
  `health`, `reaction-cleanup` (03:00 Athens, keeps 400 days of per-day counts)
  and `top-books` (today's top 10 as retained JSON on MQTT every minute).
- **Configuration**: `scheduler.threads` and `scheduler.jobs.<name>` overrides
  (schedule, enabled, timeout, jitter, retries), validated at load; an unknown
  job name stops startup with the list of known ones. A `health` section holds
  the thresholds. The old `flushIntervalMs` and `bookCacheReloadSec` became job
  schedules.
- **API**: `scheduler.list`, `scheduler.get`, `scheduler.run`,
  `scheduler.pause`, `scheduler.resume` and `system.health` (32 methods now).
- **Health** (`app::HealthMonitor`): database round trip and pool, MQTT, book
  cache entries and estimated memory, TCP server counters, like buffer, resident
  memory, threads, uptime and failing jobs. New problems are logged as warnings
  at once (then at most every 5 minutes), cleared ones as "resolved".
- `Application` became a library, so the end-to-end test now starts the real
  application instead of a hand-written copy of its wiring (the copy had already
  drifted: it had no scheduler).

Verified: 15 scheduler tests (also under ThreadSanitizer, 0 races), the
end-to-end test runs the jobs and reads the health over TCP, and a live run
against the development database showed the five jobs with the cleanup planned
for 03:00 Athens time. Pausing the test database showed the health job's warning
and, after it returned, the "resolved" lines.

## Decisions at a glance

| Decision | Made by | Why |
|---|---|---|
| Services never know the database or MQTT library | Owner (the founding goal) | Replaceable infrastructure; services testable with fakes |
| spdlog for logging | Owner suggested, agreed | Mature, fast, named loggers |
| Microsecond UTC timestamps | Assistant | Matches MariaDB; no overflow before year 9999 |
| `AppConfig` with nested sections, `config/config.json` | Owner | One place for all settings |
| Strict config (unknown keys are errors) | Assistant | Typos must not pass silently |
| MQTT behind interfaces; QoS 0 default; no flooding logs | Owner | Freedom to change the broker library |
| Last will with explicit online/offline | Owner asked; pattern by assistant | The broker alone never says "offline" on a clean stop |
| TCP: event loop + worker pool, no thread per connection | Owner | Scalability |
| JSON-RPC 2.0 + OpenRPC, code-first | Proposed by assistant, timing by owner ("at the end") | A real specification without HTTP |
| Flat categories, name-only reviewers, free tags, hard deletes, optimistic locking, page/pageSize | Owner (with defaults from the assistant) | Simplicity matching the use case |
| Per-day reaction totals + in-memory buffer | Assistant | One small transaction per second at any like rate |
| `reactionsEnabled` flag checked through a book cache | **Owner** | Explicit control over which books take likes, without a query per like |
| Days in Europe/Athens, likes − dislikes, rolling periods | Assistant defaults, accepted | Matches the audience |
| Web UI via a Node gateway, React + Vite + TS, monorepo, no login | Owner's choices on the assistant's proposal | Browsers cannot speak raw TCP |
| `include/` + `src/`, Doxygen + doxygen-awesome | Owner asked; tool chosen by assistant | Clear public API; modern-looking generated docs |
| Driver code in separate libraries | Assistant (owner: "any refactoring you want") | The build enforces the architecture |
| A reusable scheduler; jobs in `config.json`; start/stop over JSON-RPC; a health job every 15 s | Owner | Periodic work in one place, controllable while running |
| every / rate / cron schedules, no overlap, retries, timeouts reported | Assistant | Covers flushes, reloads and nightly work with one mechanism |

## Bugs found and fixed

| Bug | Found by | Fix |
|---|---|---|
| Concurrent review edits read the same old rating | Concurrency integration test | `SELECT … FOR UPDATE` (`lockById`) |
| Multi-row INSERT never finished (driver computed ids per row) | Integration test hang | Separate `insert()` and `execute()` |
| Cache writers could starve under constant reads | Test of a cache update during a like storm | Writer-preferring reader/writer lock |
| A queued MQTT message could reach a destroyed listener | Tests | Shared ownership of the listener state; shutdown order |
| `/live` page path collided with the WebSocket | Screenshots | WebSocket moved to `/ws` |
| Chart showed invented zeros and a false final drop | Screenshots | Only complete intervals since the page opened |
| Retained status could end "online" after a clean stop | MQTT integration test | Ordered online/offline publishes |
| Test inbox destroyed while a client still delivered to it | ThreadSanitizer | Handler shares ownership of the inbox |
| Lambda capturing a structured binding (C++20 only) | clang warnings | Plain variable |
| Configuration no longer found next to the moved executable | Documentation review | Search every parent directory |
| Live ranking said "no likes" when the server was unreachable | Screenshot with the server stopped | An "unavailable" state; the last list is kept |

## Still open

- The command-line tool (`caelitus` subcommands and `caelitusctl`), agreed as
  the next step after the scheduler.
- Automated tests for the web gateway and an end-to-end UI test (Playwright),
  proposed at the end of step 12.
- From the original plan: full-text search, and keeping several server instances'
  caches in sync, if ever needed.
