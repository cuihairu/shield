-- jwt.lua — HS256 JSON Web Token reference implementation (RFC 7519).
--
-- Layering note (docs/architecture-decisions.md): the runtime ships only
-- cryptographic PRIMITIVES (shield.crypto, C++/OpenSSL backed); token
-- semantics belong to the Lua business layer. This file is the worked
-- example: HS256 sign/verify composed from shield.crypto.base64url_* +
-- shield.crypto.hmac_sha256 + shield.crypto.constant_time_compare.
--
-- Usage:
--   local jwt = dofile("scripts/lib/jwt.lua")
--   local token = jwt.sign({ sub = "p1", iss = "gate", exp = os.time() + 3600 },
--                          "shared-secret")
--   local claims, code, msg = jwt.verify(token, "shared-secret",
--                                        { issuer = "gate" })
--
-- verify() returns the claims table on success, or nil plus a short error
-- code ("malformed" | "unsupported_alg" | "bad_signature" | "expired" |
-- "not_yet_valid" | "bad_issuer" | "bad_audience") and a human message.
-- Security notes: signatures are compared in constant time; the alg header
-- is pinned to HS256 (an "alg":"none" or key-confusion token is rejected
-- before any claim is trusted); exp/nbf honour an optional leeway.

local jwt = {}

-- ---------------------------------------------------------------------------
-- Minimal JSON codec (private). JWT headers/claims are small, so this keeps
-- jwt.lua self-contained instead of growing a runtime JSON API surface.
-- Object keys are emitted in sorted order for deterministic (reproducible)
-- token bytes; JSON itself treats key order as insignificant.
-- ---------------------------------------------------------------------------

local function json_escape(s)
    return (s:gsub('[%z\1-\31"\\]', function(c)
        if c == '"' then return '\\"' end
        if c == '\\' then return '\\\\' end
        if c == '\b' then return '\\b' end
        if c == '\f' then return '\\f' end
        if c == '\n' then return '\\n' end
        if c == '\r' then return '\\r' end
        if c == '\t' then return '\\t' end
        return string.format('\\u%04x', string.byte(c))
    end))
end

local function json_encode_value(v)
    local t = type(v)
    if t == "nil" then
        return "null"
    elseif t == "boolean" then
        return tostring(v)
    elseif t == "number" then
        if v ~= v or v == math.huge or v == -math.huge then
            error("cannot encode non-finite number as JSON")
        end
        if math.floor(v) == v and math.abs(v) < 2 ^ 53 then
            return string.format("%d", v)
        end
        return string.format("%.14g", v)
    elseif t == "string" then
        return '"' .. json_escape(v) .. '"'
    elseif t == "table" then
        -- Array: consecutive integer keys 1..n (any other shape is an object).
        local n = 0
        local is_array = true
        for k in pairs(v) do
            if type(k) ~= "number" or k ~= math.floor(k) or k < 1 then
                is_array = false
            else
                n = n + 1
            end
        end
        if is_array and n > 0 then
            for i = 1, n do
                if v[i] == nil then is_array = false end
            end
        end
        if is_array then
            local parts = {}
            for i = 1, n do parts[i] = json_encode_value(v[i]) end
            return "[" .. table.concat(parts, ",") .. "]"
        end
        local keys = {}
        for k in pairs(v) do keys[#keys + 1] = tostring(k) end
        table.sort(keys)
        local parts = {}
        for _, k in ipairs(keys) do
            parts[#parts + 1] = '"' .. json_escape(k) .. '":' ..
                                     json_encode_value(v[k])
        end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    error("cannot encode " .. t .. " as JSON")
end

local function json_encode(v) return json_encode_value(v) end

-- Recursive-descent JSON decoder (objects, arrays, strings with \uXXXX
-- escapes incl. surrogate pairs, numbers, true/false/null).
local function json_decode(s)
    local pos = 1
    local len = #s

    local function fail(what)
        error("json: " .. what .. " at byte " .. tostring(pos))
    end

    local function skip_ws()
        while pos <= len do
            local c = s:byte(pos)
            if c ~= 32 and c ~= 9 and c ~= 10 and c ~= 13 then break end
            pos = pos + 1
        end
    end

    local function utf8_from_cp(cp)
        if cp < 0x80 then
            return string.char(cp)
        elseif cp < 0x800 then
            return string.char(0xC0 | (cp >> 6), 0x80 | (cp & 0x3F))
        elseif cp < 0x10000 then
            return string.char(0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F),
                               0x80 | (cp & 0x3F))
        end
        return string.char(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F),
                           0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F))
    end

    local simple_escape = { ['"'] = '"', ['\\'] = '\\', ['/'] = '/',
                            b = '\b', f = '\f', n = '\n', r = '\r', t = '\t' }

    local parse_value

    local function parse_string()
        pos = pos + 1  -- opening quote
        local out = {}
        while true do
            if pos > len then fail("unterminated string") end
            local c = s:byte(pos)
            if c == 34 then  -- "
                pos = pos + 1
                return table.concat(out)
            elseif c == 92 then  -- backslash
                local e = s:sub(pos + 1, pos + 1)
                if simple_escape[e] then
                    out[#out + 1] = simple_escape[e]
                    pos = pos + 2
                elseif e == "u" then
                    local hex = s:sub(pos + 2, pos + 5)
                    if #hex < 4 then fail("bad \\u escape") end
                    local cp = tonumber(hex, 16)
                    if not cp then fail("bad \\u escape") end
                    pos = pos + 6
                    if cp >= 0xD800 and cp <= 0xDBFF and s:sub(pos, pos + 1) == "\\u" then
                        local lo = tonumber(s:sub(pos + 2, pos + 5), 16)
                        if lo and lo >= 0xDC00 and lo <= 0xDFFF then
                            cp = 0x10000 + (cp - 0xD800) * 0x400 + (lo - 0xDC00)
                            pos = pos + 6
                        end
                    end
                    out[#out + 1] = utf8_from_cp(cp)
                else
                    fail("bad escape")
                end
            else
                local start = pos
                repeat pos = pos + 1 until pos > len or s:byte(pos) == 34 or
                    s:byte(pos) == 92
                out[#out + 1] = s:sub(start, pos - 1)
            end
        end
    end

    local function parse_number()
        local _, epos = s:find("^-?%d+%.?%d*[eE]?[-+]?%d*", pos)
        if not epos then fail("bad number") end
        local num = s:sub(pos, epos)
        pos = epos + 1
        return tonumber(num)
    end

    parse_value = function()
        skip_ws()
        if pos > len then fail("unexpected end") end
        local c = s:sub(pos, pos)
        if c == "{" then
            pos = pos + 1
            local obj = {}
            skip_ws()
            if s:sub(pos, pos) == "}" then
                pos = pos + 1
                return obj
            end
            while true do
                skip_ws()
                if s:sub(pos, pos) ~= '"' then fail("expected key") end
                local k = parse_string()
                skip_ws()
                if s:sub(pos, pos) ~= ":" then fail("expected :") end
                pos = pos + 1
                obj[k] = parse_value()
                skip_ws()
                local sep = s:sub(pos, pos)
                if sep == "," then
                    pos = pos + 1
                elseif sep == "}" then
                    pos = pos + 1
                    return obj
                else
                    fail("expected , or }")
                end
            end
        elseif c == "[" then
            pos = pos + 1
            local arr = {}
            skip_ws()
            if s:sub(pos, pos) == "]" then
                pos = pos + 1
                return arr
            end
            while true do
                arr[#arr + 1] = parse_value()
                skip_ws()
                local sep = s:sub(pos, pos)
                if sep == "," then
                    pos = pos + 1
                elseif sep == "]" then
                    pos = pos + 1
                    return arr
                else
                    fail("expected , or ]")
                end
            end
        elseif c == '"' then
            return parse_string()
        elseif s:sub(pos, pos + 3) == "true" then
            pos = pos + 4
            return true
        elseif s:sub(pos, pos + 4) == "false" then
            pos = pos + 5
            return false
        elseif s:sub(pos, pos + 3) == "null" then
            pos = pos + 4
            return nil
        else
            return parse_number()
        end
    end

    local value = parse_value()
    skip_ws()
    if pos <= len then fail("trailing content") end
    return value
end

-- ---------------------------------------------------------------------------
-- HS256 sign / verify
-- ---------------------------------------------------------------------------

local function b64url_encode(raw)
    return shield.crypto.base64url_encode(raw)
end

local function b64url_decode(seg)
    local ok, raw = pcall(shield.crypto.base64url_decode, seg)
    if not ok then return nil end
    return raw
end

local function json_decode_segment(seg)
    local raw = b64url_decode(seg)
    if not raw then return nil end
    local ok, value = pcall(json_decode, raw)
    if not ok or type(value) ~= "table" then return nil end
    return value
end

-- jwt.sign(claims, key [, opts]) -> token
-- opts.header overrides the default {alg="HS256", typ="JWT"} header (claims
-- inside it are still validated by verify to be HS256; anything else is a
-- claim-set the receiver will reject by default).
function jwt.sign(claims, key, opts)
    assert(type(claims) == "table", "claims must be a table")
    assert(type(key) == "string" and #key > 0,
           "key must be a non-empty string")
    local header = { alg = "HS256", typ = "JWT" }
    if opts and type(opts) == "table" and type(opts.header) == "table" then
        header = opts.header
        header.alg = header.alg or "HS256"
    end
    local signing_input = b64url_encode(json_encode(header)) .. "." ..
                              b64url_encode(json_encode(claims))
    local sig = b64url_encode(shield.crypto.hmac_sha256(key, signing_input))
    return signing_input .. "." .. sig
end

-- jwt.verify(token, key [, opts]) -> claims | nil, code, message
-- opts: now (unix seconds override; default os.time()), leeway (seconds,
-- default 0), issuer, audience.
function jwt.verify(token, key, opts)
    opts = opts or {}
    if type(token) ~= "string" then
        return nil, "malformed", "token must be a string"
    end
    if type(key) ~= "string" or #key == 0 then
        return nil, "malformed", "key must be a non-empty string"
    end
    local parts = {}
    for seg in (token .. "."):gmatch("([^.]*)%.") do
        parts[#parts + 1] = seg
    end
    if #parts ~= 3 or parts[1] == "" or parts[2] == "" or parts[3] == "" then
        return nil, "malformed", "token must have 3 non-empty segments"
    end

    local header = json_decode_segment(parts[1])
    if not header then
        return nil, "malformed", "undecodable header"
    end
    -- Pin the algorithm before trusting anything else: an unsigned
    -- ("alg":"none") or algorithm-confusion token must never verify.
    if header.alg ~= "HS256" then
        return nil, "unsupported_alg", "only HS256 is supported"
    end

    local expected = b64url_encode(
                          shield.crypto.hmac_sha256(key,
                                                    parts[1] .. "." .. parts[2]))
    if not shield.crypto.constant_time_compare(parts[3], expected) then
        return nil, "bad_signature", "signature mismatch"
    end

    local claims = json_decode_segment(parts[2])
    if not claims then
        return nil, "malformed", "undecodable payload"
    end

    local now = opts.now or os.time()
    local leeway = opts.leeway or 0
    if claims.exp ~= nil then
        if type(claims.exp) ~= "number" then
            return nil, "malformed", "exp must be a number"
        end
        if now - leeway > claims.exp then
            return nil, "expired", "token has expired"
        end
    end
    if claims.nbf ~= nil then
        if type(claims.nbf) ~= "number" then
            return nil, "malformed", "nbf must be a number"
        end
        if now + leeway < claims.nbf then
            return nil, "not_yet_valid", "token is not yet valid"
        end
    end
    if opts.issuer ~= nil and claims.iss ~= opts.issuer then
        return nil, "bad_issuer", "issuer mismatch"
    end
    if opts.audience ~= nil then
        local aud, matches = claims.aud, false
        if type(aud) == "string" then
            matches = aud == opts.audience
        elseif type(aud) == "table" then
            for _, v in ipairs(aud) do
                if v == opts.audience then
                    matches = true
                    break
                end
            end
        end
        if not matches then
            return nil, "bad_audience", "audience mismatch"
        end
    end
    return claims
end

return jwt
