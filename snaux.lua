local m = {}
package.loaded[...] = m



local c = require "skynet.core"
local profile = require "profile"

local coroutine_resume = profile.resume
local coroutine_yield = profile.yield



function m.wait_c_response(session)
	local succ, msg, sz = coroutine_yield('CALL', session)
	--TODO unacpk ccall msg, sz
end
