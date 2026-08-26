-- bench_json.lua: wrk JSON reporter for KEEL benchmarks
-- Usage: wrk --latency -s bench_json.lua http://...

done = function(summary, latency, requests)
    io.write(string.format(
        '{"rps":%.0f,"latency_avg_us":%.0f,"latency_max_us":%.0f,' ..
        '"latency_p50_us":%.0f,"latency_p90_us":%.0f,"latency_p99_us":%.0f,' ..
        '"requests":%d,"errors_connect":%d,"errors_read":%d,' ..
        '"errors_write":%d,"errors_timeout":%d}',
        summary.requests / (summary.duration / 1e6),
        latency.mean,
        latency.max,
        latency:percentile(50),
        latency:percentile(90),
        latency:percentile(99),
        summary.requests,
        summary.errors.connect,
        summary.errors.read,
        summary.errors.write,
        summary.errors.timeout
    ))
end
