-- post_body.lua: wrk POST body for KEEL /echo benchmark

wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.body = '{"name":"benchmark","value":42}'
