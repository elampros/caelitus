/// @file
/// The scheduler methods (`scheduler.list`, `get`, `run`, `pause`, `resume`),
/// `system.health`, and their result schemas.
/// @ingroup api

#include "caelitus/api/OperationsApi.hpp"

#include "caelitus/api/Schema.hpp"
#include "caelitus/core/DomainErrors.hpp"

namespace caelitus::api {

namespace {

namespace S = schema;

Json optionalTime(const std::optional<Timestamp>& t) { return t ? Json(*t) : Json(nullptr); }

Json toJson(const scheduler::JobStatus& j) {
    return {{"name", j.name},
            {"description", j.description},
            {"schedule", j.schedule},
            {"paused", j.paused},
            {"running", j.running},
            {"nextRun", optionalTime(j.nextRun)},
            {"lastStart", optionalTime(j.lastStart)},
            {"lastEnd", optionalTime(j.lastEnd)},
            {"lastDurationMs", j.lastDurationMs ? Json(*j.lastDurationMs) : Json(nullptr)},
            {"lastResult", j.lastResult},
            {"lastError", j.lastError.empty() ? Json(nullptr) : Json(j.lastError)},
            {"runs", j.runs},
            {"failures", j.failures},
            {"consecutiveFailures", j.consecutiveFailures}};
}

Json toJson(const HealthReport& h) {
    return {{"status", h.status},
            {"problems", h.problems},
            {"checkedAt", h.checkedAt},
            {"uptimeSeconds", h.uptimeSeconds},
            {"version", h.version},
            {"database",
             {{"up", h.database.up},
              {"pingMs", h.database.pingMs ? Json(*h.database.pingMs) : Json(nullptr)},
              {"openConnections", h.database.openConnections},
              {"idleConnections", h.database.idleConnections},
              {"maxConnections", h.database.maxConnections}}},
            {"mqtt",
             {{"connected", h.mqtt.connected},
              {"published", h.mqtt.published},
              {"publishDropped", h.mqtt.publishDropped},
              {"received", h.mqtt.received},
              {"receiveDropped", h.mqtt.receiveDropped}}},
            {"bookCache",
             {{"books", h.bookCache.books},
              {"approxBytes", h.bookCache.approxBytes},
              {"hits", h.bookCache.hits},
              {"misses", h.bookCache.misses}}},
            {"server",
             {{"activeConnections", h.server.activeConnections},
              {"totalConnections", h.server.totalConnections},
              {"requests", h.server.requests},
              {"handlerErrors", h.server.handlerErrors},
              {"protocolErrors", h.server.protocolErrors}}},
            {"reactions",
             {{"pending", h.reactions.pending}, {"capacity", h.reactions.capacity}, {"dropped", h.reactions.dropped}}},
            {"process",
             {{"memoryBytes", h.process.memoryBytes ? Json(*h.process.memoryBytes) : Json(nullptr)},
              {"threads", h.process.threads ? Json(*h.process.threads) : Json(nullptr)}}},
            {"jobs",
             {{"total", h.jobs.total},
              {"paused", h.jobs.paused},
              {"running", h.jobs.running},
              {"failing", h.jobs.failing}}}};
}

void registerSchemas(JsonRpcHandler& rpc) {
    const Json time = S::nullable(S::dateTime());
    rpc.addSchema("Job",
                  S::describe(S::object({
                                  {"name", S::describe(S::string(1), "Unique job name")},
                                  {"description", S::string()},
                                  {"schedule",
                                   S::describe(S::string(), "e.g. \"every 15s\", \"cron 0 3 * * * (Europe/Athens)\"")},
                                  {"paused", S::describe(S::boolean(), "Paused jobs only run when started by hand")},
                                  {"running", S::boolean()},
                                  {"nextRun", S::describe(time, "Next planned start; null while paused or running")},
                                  {"lastStart", time},
                                  {"lastEnd", time},
                                  {"lastDurationMs", S::nullable(S::number(0))},
                                  {"lastResult", S::enumOf({"never", "ok", "failed"})},
                                  {"lastError", S::describe(S::nullable(S::string()), "Message of the last failure")},
                                  {"runs", S::integer(0)},
                                  {"failures", S::integer(0)},
                                  {"consecutiveFailures",
                                   S::describe(S::integer(0), "Failures since the last success")},
                              }),
                              "A scheduled job: its schedule, state and history"));
    const Json count = S::integer(0);
    rpc.addSchema(
        "HealthReport",
        S::describe(S::object({
                        {"status", S::describe(S::enumOf({"ok", "degraded"}), "degraded when problems is not empty")},
                        {"problems", S::describe(S::array(S::string()), "What is wrong, one sentence each")},
                        {"checkedAt", S::dateTime()},
                        {"uptimeSeconds", count},
                        {"version", S::string()},
                        {"database", S::object({{"up", S::boolean()},
                                                {"pingMs", S::nullable(S::number(0))},
                                                {"openConnections", count},
                                                {"idleConnections", count},
                                                {"maxConnections", count}})},
                        {"mqtt", S::object({{"connected", S::boolean()},
                                            {"published", count},
                                            {"publishDropped", count},
                                            {"received", count},
                                            {"receiveDropped", count}})},
                        {"bookCache", S::object({{"books", count},
                                                 {"approxBytes", S::describe(count, "Estimated memory use")},
                                                 {"hits", count},
                                                 {"misses", count}})},
                        {"server", S::object({{"activeConnections", count},
                                              {"totalConnections", count},
                                              {"requests", count},
                                              {"handlerErrors", count},
                                              {"protocolErrors", count}})},
                        {"reactions",
                         S::object({{"pending", S::describe(count, "Book-days waiting for the next flush")},
                                    {"capacity", count},
                                    {"dropped", count}})},
                        {"process",
                         S::object({{"memoryBytes", S::describe(S::nullable(count), "Resident memory (RSS)")},
                                    {"threads", S::nullable(count)}})},
                        {"jobs",
                         S::object({{"total", count}, {"paused", count}, {"running", count}, {"failing", count}})},
                    }),
                    "The latest health check of every part of the server"));
}

scheduler::Scheduler& need(const std::shared_ptr<scheduler::Scheduler>& s) {
    if (!s) throw std::logic_error("scheduler not available");
    return *s;
}

Json jobOrThrow(scheduler::Scheduler& s, const std::string& name) {
    auto status = s.status(name);
    if (!status) throw NotFoundError("job", name);
    return toJson(*status);
}

}  // namespace

void registerOperationsApi(JsonRpcHandler& rpc, std::shared_ptr<scheduler::Scheduler> sched, HealthSource health) {
    registerSchemas(rpc);
    const Json name = S::describe(S::string(1, 64), "Job name, as listed by scheduler.list");

    MethodBuilder(rpc, "scheduler.list", "scheduler", "Every scheduled job with its state and history.")
        .description("Jobs run inside the server on schedules from the configuration (scheduler.jobs).")
        .returns("jobs", S::array(S::ref("Job")))
        .handler([sched](const Params&) {
            Json out = Json::array();
            for (const auto& j : need(sched).list()) out.push_back(toJson(j));
            return out;
        });

    MethodBuilder(rpc, "scheduler.get", "scheduler", "One scheduled job.")
        .required("name", name, "Job name")
        .returns("job", S::ref("Job"))
        .errors({"NotFound"})
        .handler([sched](const Params& p) { return jobOrThrow(need(sched), p.required<std::string>("name")); });

    MethodBuilder(rpc, "scheduler.run", "scheduler", "Runs a job now.")
        .description(
            "Also while the job is paused; its regular schedule is not changed. The run starts in the "
            "background: the result shows running = true, and scheduler.get shows the outcome.")
        .required("name", name, "Job name")
        .returns("job", S::ref("Job"))
        .errors({"NotFound"})
        .conflicts({"job_running"})
        .handler([sched](const Params& p) {
            const auto n = p.required<std::string>("name");
            switch (need(sched).runNow(n)) {
                case scheduler::Scheduler::RunResult::NotFound: throw NotFoundError("job", n);
                case scheduler::Scheduler::RunResult::AlreadyRunning:
                    throw ConflictError("job_running", "Job '" + n + "' is already running");
                case scheduler::Scheduler::RunResult::Started: break;
            }
            return jobOrThrow(*sched, n);
        });

    MethodBuilder(rpc, "scheduler.pause", "scheduler", "Stops a job's scheduled runs.")
        .description("A run in progress finishes. Not persisted: after a restart the configuration decides.")
        .required("name", name, "Job name")
        .returns("job", S::ref("Job"))
        .errors({"NotFound"})
        .handler([sched](const Params& p) {
            const auto n = p.required<std::string>("name");
            if (!need(sched).pause(n)) throw NotFoundError("job", n);
            return jobOrThrow(*sched, n);
        });

    MethodBuilder(rpc, "scheduler.resume", "scheduler", "Resumes a paused job.")
        .description("Its next run is planned from now.")
        .required("name", name, "Job name")
        .returns("job", S::ref("Job"))
        .errors({"NotFound"})
        .handler([sched](const Params& p) {
            const auto n = p.required<std::string>("name");
            if (!need(sched).resume(n)) throw NotFoundError("job", n);
            return jobOrThrow(*sched, n);
        });

    MethodBuilder(rpc, "system.health", "system", "The server's latest health report.")
        .description(
            "Made by the \"health\" job (default every 15 s): database, MQTT, book cache, TCP server, "
            "like buffer, memory, threads and jobs, with a list of problems. Unavailable until the first "
            "check after startup.")
        .returns("health", S::ref("HealthReport"))
        .handler([health](const Params&) {
            std::optional<HealthReport> report = health ? health() : std::nullopt;
            if (!report) throw std::logic_error("no health report yet");
            return toJson(*report);
        });
}

}  // namespace caelitus::api
