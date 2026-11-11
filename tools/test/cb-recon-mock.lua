-- Teste simulado do mod cb-recon (recon-kit/cb-recon/Scripts/main.lua) sem o jogo: imita a API Lua do UE4SS com
-- objetos falsos, relógio falso e fila de tarefas, e confere o fluxo (espera, menu ignorado, relatório, dumpers uma
-- vez, erro de dumper registrado, etapa travada pulada na sessão seguinte).
--
-- Uso: build\lua54\lua.exe tools\test\cb-recon-mock.lua <pasta-temporária-vazia> <caminho do main.lua>
-- (o tools/make-recon-kit.mjs chama assim e depois valida cada relatorio-*.json com JSON.parse em UTF-8 estrito).

local tmp, mainSrc = arg[1], arg[2]
assert(tmp and mainSrc, "uso: cb-recon-mock.lua <tmp> <main.lua>")
local modDir = tmp .. "\\Mods\\cb-recon"
local scriptPath = modDir .. "\\Scripts\\main.lua"
os.execute('mkdir "' .. modDir .. '\\Scripts" >nul 2>&1')
do
    local src = assert(io.open(mainSrc, "rb")):read("a")
    local f = assert(io.open(scriptPath, "wb")); f:write(src); f:close()
end

local failures = 0
local function check(cond, msg)
    if cond then print("ok   " .. msg) else print("FALHA " .. msg); failures = failures + 1 end
end

-- relógio falso
local clock = 1000000
local realTime = os.time
os.time = function(t) if t then return realTime(t) end return clock end

-- objetos falsos ----------------------------------------------------------------------------------------------------
local nextAddr = 0x1000
local Obj = {}
local function new(kind, full, fname, cls, fields)
    nextAddr = nextAddr + 0x10
    local o = { __kind = kind, __full = full, __fname = fname, __class = cls, __fields = fields or {},
        __addr = nextAddr, __valid = true }
    return setmetatable(o, Obj)
end
local methods = {}
Obj.__index = function(o, k)
    if methods[k] then return methods[k] end
    local f = rawget(o, "__fields")
    if f[k] ~= nil then
        if f[k] == "THROW" then error("propriedade " .. k .. " explodiu") end
        return f[k]
    end
    error("membro inexistente: " .. tostring(k))
end
function methods.IsValid(o) return o.__valid end
function methods.GetFullName(o) return o.__full end
function methods.GetFName(o) return { ToString = function() return o.__fname end } end
function methods.GetAddress(o) return o.__addr end
function methods.GetClass(o) return o.__class end
local INVALID = setmetatable({ __kind = "UClass", __full = "", __fname = "", __fields = {}, __addr = 0, __valid = false }, Obj)
function methods.GetSuperStruct(o) return rawget(o, "__fields").__super or INVALID end
function methods.type(o) return o.__kind end
function methods.GetOuter(o) return rawget(o, "__fields").__outer end
function methods.GetWorld(o) return rawget(o, "__fields").__world end
function methods.IsClass(o) return o.__kind == "UClass" end
function methods.IsA(o, cls)
    local c = o.__class
    while c and c.__valid do
        if c == cls then return true end
        c = rawget(c, "__fields").__super
    end
    return false
end
function methods.ForEachProperty(o, cb)
    for _, p in ipairs(rawget(o, "__fields").__props or {}) do
        local prop = { GetFName = function() return { ToString = function() return p[1] end } end,
            GetClass = function() return { GetFName = function() return { ToString = function() return p[2] end } end } end }
        if cb(prop) then return end
    end
end
function methods.IsLocalPlayerController(o) return true end
function methods.K2_GetActorLocation(o)
    return setmetatable({ X = 1.5, Y = -2, Z = 300 }, { __index = { type = function() return "UScriptStruct" end } })
end
function methods.K2_GetActorRotation(o)
    return setmetatable({ Pitch = 0, Yaw = 90, Roll = 0 }, { __index = { type = function() return "UScriptStruct" end } })
end
function methods.GetAnimInstance(o) return rawget(o, "__fields").__anim end
function methods.GetAttachParentActor(o) return rawget(o, "__fields").__attachParent end

local function class(path, fname, super, props)
    return new("UClass", "Class " .. path, fname, nil, { __super = super, __props = props })
end
local CObject = class("/Script/CoreUObject.Object", "Object")
local CActor = class("/Script/Engine.Actor", "Actor", CObject)
local CPawn = class("/Script/Engine.Pawn", "Pawn", CActor)
local CCharacter = class("/Script/Engine.Character", "Character", CPawn)
local CBiped = class("/Script/TestGame.TestCharBase", "TestCharBase", CCharacter,
    { { "Health", "FloatProperty" }, { "Gear", "ObjectProperty" }, { "Broken", "FloatProperty" }, { "OnHit", "MulticastDelegateProperty" } })
local CPlayer = class("/Script/TestGame.TestPlayerChar", "TestPlayerChar", CBiped, { { "PlayerId", "IntProperty" }, { "Apelido", "StrProperty" } })
local CController = class("/Script/Engine.PlayerController", "PlayerController", CActor)
local CPhxController = class("/Script/TestGame.TestPlayerController", "TestPlayerController", CController,
    { { "bInMenu", "BoolProperty" } })
local CActorComp = class("/Script/Engine.ActorComponent", "ActorComponent", CObject)
local CScene = class("/Script/Engine.SceneComponent", "SceneComponent", CActorComp)
local CPrim = class("/Script/Engine.PrimitiveComponent", "PrimitiveComponent", CScene)
local CSkel = class("/Script/Engine.SkeletalMeshComponent", "SkeletalMeshComponent", CPrim)
local CMove = class("/Script/Engine.CharacterMovementComponent", "CharacterMovementComponent", CActorComp)
local CAppear = class("/Script/TestGame.TestLookComponent", "TestLookComponent", CActorComp,
    { { "HeadId", "NameProperty" } })
local CAnim = class("/Script/TestGame.TestAnimInstance", "TestAnimInstance", CObject, { { "Speed", "FloatProperty" } })
local CTestHeldTool = class("/Script/TestGame.TestHeldTool", "TestHeldTool", CActor)
local CWorld = class("/Script/Engine.World", "World", CObject)

local function world(name)
    return new("UWorld", "World /Game/Maps/" .. name .. "." .. name, name, CWorld,
        { PersistentLevel = new("UObject", "Level /Game/Maps/" .. name .. ".PersistentLevel", "PersistentLevel", nil) })
end

local state = {}
local function makeScene(wname, pawnClass)
    -- troca de mapa: os objetos da cena anterior deixam de valer
    for _, o in ipairs(state.actors or {}) do o.__valid = false end
    for _, o in ipairs(state.components or {}) do o.__valid = false end
    local w = world(wname)
    local pc = new("UObject", "TestPlayerController /Game/Maps/" .. wname .. ".PC_0", "PC_0", CPhxController,
        { __world = w, bInMenu = false })
    local anim = new("UObject", "TestAnimInstance /Transient.Anim_0", "Anim_0", CAnim, { Speed = 0.0 })
    local pawn = new("AActor", "TestPlayerChar /Game/Maps/" .. wname .. ".Player_0", "Player_0", pawnClass or CPlayer,
        { __world = w, Health = 100.0, Broken = "THROW", PlayerId = 7,
          Apelido = { type = function() return "FString" end, ToString = function() return "a" .. string.rep("ã", 300) end } })
    local mesh = new("UObject", "SkeletalMeshComponent Player_0.Mesh", "Mesh", CSkel,
        { __outer = pawn, __anim = anim, SkeletalMesh = new("UObject", "SkeletalMesh /Game/Char/SK_Body.SK_Body", "SK_Body", nil) })
    local move = new("UObject", "CharacterMovementComponent Player_0.CharMoveComp", "CharMoveComp", CMove,
        { __outer = pawn, MaxWalkSpeed = 420.0, MovementMode = 1 })
    local appear = new("UObject", "TestLookComponent Player_0.Customization", "Customization", CAppear,
        { __outer = pawn, HeadId = { type = function() return "FName" end, ToString = function() return "Head_03" end } })
    pawn.__fields.Mesh = mesh
    pawn.__fields.CharacterMovement = move
    pc.__fields.Pawn = pawn
    local wand = new("AActor", "TestHeldTool /Game/Maps/" .. wname .. ".Wand_0", "Wand_0", CTestHeldTool,
        { __world = w, Owner = pawn })
    state.pc, state.pawn, state.world = pc, pawn, w
    state.components = { mesh, move, appear }
    state.actors = { pawn, pc, wand }
end

local classesAll = { CObject, CActor, CPawn, CCharacter, CBiped, CPlayer, CController, CPhxController, CActorComp, CScene,
    CPrim, CSkel, CMove, CAppear, CAnim, CTestHeldTool, CWorld }

-- API UE4SS falsa --------------------------------------------------------------------------------------------------
local calls = {}
local function resetApi()
    calls = {}
    _G.FindAllOf = function(name)
        if name == "PlayerController" then return state.pc and { state.pc } or nil end
        if name == "ActorComponent" then return state.components end
        if name == "Actor" then return state.actors end
        if name == "Class" then return classesAll end
        return nil
    end
    _G.FindFirstOf = function(name) return new("UObject", name .. " /Engine/Transient." .. name .. "_0", name, nil) end
    _G.StaticFindObject = function(path)
        for _, c in ipairs(classesAll) do if c.__full == "Class " .. path then return c end end
        return new("UObject", "", "", nil, {}) -- inválido abaixo
    end
    local inv = StaticFindObject
    _G.StaticFindObject = function(p) local o = inv(p); if o.__full == "" then o.__valid = false end return o end
    _G.UE4SS = { GetVersion = function() return 3, 0, 1 end }
    _G.UnrealVersion = { GetMajor = function() return 4 end, GetMinor = function() return 27 end }
    for _, fn in ipairs({ "DumpAllObjects", "GenerateUHTCompatibleHeaders", "GenerateSDK", "DumpUSMAP", "DumpAllActors",
        "GenerateLuaTypes" }) do
        _G[fn] = function()
            calls[#calls + 1] = fn
            if fn == "GenerateSDK" then error("falha simulada do gerador de SDK") end
        end
    end
end

local tasks, nextHandle = {}, 1
local function addTask(delay, fn, interval)
    local h = nextHandle; nextHandle = nextHandle + 1
    tasks[#tasks + 1] = { due = clock + delay / 1000, fn = fn, interval = interval, h = h }
    return h
end
_G.ExecuteInGameThreadWithDelay = function(ms, fn) return addTask(ms, fn, nil) end
_G.LoopInGameThreadWithDelay = function(ms, fn) return addTask(ms, fn, ms) end
_G.CancelDelayedAction = function(h)
    for i = #tasks, 1, -1 do if tasks[i].h == h then table.remove(tasks, i) end end
end

local function runFor(seconds, onSecond)
    for _ = 1, seconds do
        clock = clock + 1
        if onSecond then onSecond(clock) end
        local due = {}
        for _, t in ipairs(tasks) do if t.due <= clock then due[#due + 1] = t end end
        for _, t in ipairs(due) do
            if t.interval then t.due = clock + t.interval / 1000 else
                for i = #tasks, 1, -1 do if tasks[i] == t then table.remove(tasks, i) end end
            end
            t.fn()
        end
    end
end

local function readAll(p) local f = io.open(p, "rb"); if not f then return nil end local s = f:read("a"); f:close(); return s end
local function listReports()
    local out = {}
    local p = io.popen('dir /b "' .. modDir .. '\\relatorio-*.json" 2>nul')
    for l in p:lines() do out[#out + 1] = l end
    p:close()
    return out
end

-- Sessão 1: menu por 60 s (Character num mundo "MainMenu"), depois o mundo de verdade -------------------------------
resetApi()
makeScene("MainMenu_P")
local START1 = clock
dofile(scriptPath)
runFor(60)
check(#calls == 0 and #listReports() == 0, "sessão 1: nada roda enquanto o mundo parece menu")
makeScene("WorldA")
runFor(40)
check(#listReports() == 0, "sessão 1: espera a estabilidade antes do relatório")
runFor(120)
local reps = listReports()
check(#reps == 1, "sessão 1: um relatório gravado (" .. #reps .. ")")
local expected = { "DumpAllObjects", "GenerateUHTCompatibleHeaders", "GenerateSDK", "DumpUSMAP", "DumpAllActors",
    "GenerateLuaTypes" }
check(table.concat(calls, ",") == table.concat(expected, ","), "sessão 1: dumpers na ordem, uma vez cada: " .. table.concat(calls, ","))
local log1 = readAll(modDir .. "\\cb-recon.log") or ""
check(log1:find("TUDO PRONTO", 1, true) ~= nil, "sessão 1: log avisa TUDO PRONTO")
check(log1:find("falha simulada do gerador de SDK", 1, true) ~= nil, "sessão 1: erro do GenerateSDK registrado no log")
local st = readAll(modDir .. "\\estado.txt") or ""
check(st:find("sdk_cxx ERRO", 1, true) ~= nil and st:find("objetos OK", 1, true) ~= nil, "sessão 1: estado.txt com OK/ERRO")
local rep = reps[1] and readAll(modDir .. "\\" .. reps[1]) or ""
check(rep:find("TestPlayerChar", 1, true) and rep:find("TestLookComponent", 1, true)
    and rep:find("SK_Body", 1, true) and rep:find("TestHeldTool", 1, true) and rep:find("Head_03", 1, true)
    and rep:find("TestAnimInstance", 1, true) and rep:find("WorldA", 1, true) ~= nil,
    "sessão 1: relatório tem pawn, componentes, malha, anim, ator ligado e mundo")
check(rep:find("explodiu", 1, true) ~= nil, "sessão 1: propriedade que dá erro vira 'erro' no relatório, sem parar")
runFor(30 * 60)
check(#calls == #expected, "sessão 1: nada repete depois")
-- mudança de mundo gera relatório extra
makeScene("WorldB")
runFor(120)
check(#listReports() >= 2, "sessão 1: relatório extra ao mudar de mundo")

-- Sessão 2: simula um travamento no meio do DumpUSMAP numa sessão anterior ------------------------------------------
do
    local s = readAll(modDir .. "\\estado.txt")
    s = s:gsub("[^\n]* usmap OK[^\n]*\n", "")
    local f = io.open(modDir .. "\\estado.txt", "wb"); f:write(s, "2026-01-01 00:00:00 usmap INICIADO\n"); f:close()
end
tasks = {}
resetApi()
makeScene("WorldA")
dofile(scriptPath)
runFor(200)
check(#calls == 0, "sessão 2: nenhum dumper repetido; o travado é pulado (" .. table.concat(calls, ",") .. ")")
st = readAll(modDir .. "\\estado.txt") or ""
check(st:find("usmap PULADO", 1, true) ~= nil, "sessão 2: etapa travada marcada PULADO")

-- Sessão 3: shaders/menu demorados não contam; tempo esgotado no mundo não desiste ----------------------------------
tasks = {}
resetApi()
state.pc = nil
os.remove(modDir .. "\\estado.txt")
os.remove(modDir .. "\\cb-recon.log")
dofile(scriptPath)
runFor(15 * 60)                       -- "compilando shaders": nenhum controller
makeScene("MainMenu_P")
runFor(25 * 60)                       -- menu muito longo
local log3 = readAll(modDir .. "\\cb-recon.log") or ""
check(log3:find("tempo esgotado", 1, true) == nil and #calls == 0, "sessão 3: 40 min sem controller/no menu não contam no limite")
makeScene("WorldA", CPawn)            -- mundo de verdade, mas Pawn que não é Character (nunca vira candidato)
runFor(31 * 60)
log3 = readAll(modDir .. "\\cb-recon.log") or ""
check(log3:find("tempo esgotado", 1, true) ~= nil, "sessão 3: timeout registrado 30 min depois do primeiro mundo fora de menu")
check(#calls == 0, "sessão 3: sem jogador válido, nenhum dumper roda")
makeScene("WorldA")                   -- o jogador aparece depois do timeout: continua esperando e roda tudo
runFor(200)
check(#calls == 6, "sessão 3: depois do timeout o mod continua esperando e roda os dumpers (" .. #calls .. ")")
local reps3 = listReports()
local anyVer = false
for _, r in ipairs(reps3) do if (readAll(modDir .. "\\" .. r) or ""):find('"ue4ss": "3.0.1"', 1, true) then anyVer = true end end
check(anyVer, "sessão 3: versão do UE4SS completa no relatório (3.0.1)")

print(failures == 0 and "TODOS OK" or (failures .. " FALHA(S)"))
os.exit(failures == 0 and 0 or 1)
