-- cb-recon: mod Lua do UE4SS para o kit de reconhecimento (docs/RECONHECIMENTO-JOGO.md).
--
-- Depois que o jogador local está no mundo (PlayerController local com Pawn do tipo Character, estável por um tempo),
-- grava um relatório JSON sobre o jogador e roda, uma etapa por vez, os dumpers do próprio UE4SS. Sem teclas.
--
-- Regras:
--   * Tudo dentro de pcall/xpcall: um erro vira linha no log, nunca derruba o jogo pelo Lua.
--   * Roda uma vez por sessão do jogo. As etapas pesadas (dumpers) rodam uma vez por instalação: o estado fica em
--     estado.txt. Uma etapa que começou e não terminou numa sessão anterior (o jogo fechou/travou no meio) é pulada.
--   * Arquivos gravados só na pasta deste mod (ue4ss\Mods\cb-recon\). Os dumpers do UE4SS gravam na pasta ue4ss\.
--
-- Nomes das funções do UE4SS conferidos no fonte vendorizado (client/deps/RE-UE4SS, commit 4fb9b61):
--   UE4SS/src/Mod/LuaMod.cpp registra DumpAllObjects, GenerateSDK, GenerateLuaTypes, GenerateUHTCompatibleHeaders,
--   DumpAllActors, DumpUSMAP, FindAllOf, StaticFindObject, ExecuteInGameThreadWithDelay, LoopInGameThreadWithDelay,
--   CancelDelayedAction, ExecuteWithDelay, LoopAsync, ExecuteInGameThread.

local VERSION = "0.1.0"

local CFG = {
    pollMs = 2000,             -- intervalo da espera pelo jogador
    stableSeconds = 45,        -- o mesmo Pawn por este tempo = jogador no mundo
    timeoutSeconds = 30 * 60,  -- conta só a partir do primeiro mundo fora de menu com controller local; ao esgotar
                               -- grava um relatório parcial, avisa no log e CONTINUA esperando (compilação de shaders,
                               -- vídeos e menu não contam)
    statusEverySeconds = 30,   -- linha de "aguardando" no log
    gapMs = 4000,              -- pausa entre etapas (o jogo respira e o log mostra o progresso)
    maxStr = 400,
    maxPerPattern = 300,
    maxClassesScanned = 400000,
    maxPropsPerObject = 160,
    maxComponents = 200,
    maxOwnedActors = 100,
    maxArrayPreview = 10,
    maxStreamingLevels = 200,
}

-- Mundo com cara de menu/tela inicial: não conta como "no jogo".
local MENU_WORLD_HINTS = { "menu", "frontend", "front_end", "title", "startup", "entry", "boot", "splash", "intro" }

-- Classes carregadas cujo nome curto contém algum destes trechos (sensível a maiúsculas, como os nomes do UE).
local PATTERNS = {
    "Biped", "Player", "Spell", "Wand", "Anim", "Appearance", "Customization", "Customisation", "NPC", "Mission",
    "Quest", "TimeOfDay", "Weather", "Broom", "Mount", "Gear", "Outfit", "Cloth",
}

-- Etapas pesadas, na ordem. "fn" = nome da função global do UE4SS.
local DUMP_STEPS = {
    { id = "objetos",     fn = "DumpAllObjects",               saida = "ue4ss\\UE4SS_ObjectDump.txt" },
    { id = "uht",         fn = "GenerateUHTCompatibleHeaders", saida = "ue4ss\\UHTHeaderDump\\" },
    { id = "sdk_cxx",     fn = "GenerateSDK",                  saida = "ue4ss\\CXXHeaderDump\\" },
    { id = "usmap",       fn = "DumpUSMAP",                    saida = "ue4ss\\*.usmap" },
    { id = "atores",      fn = "DumpAllActors",                saida = "ue4ss\\<n>-ue4ss_actor_data.csv" },
    { id = "tipos_lua",   fn = "GenerateLuaTypes",             saida = "ue4ss\\Mods\\shared\\types\\" },
}

---------------------------------------------------------------------------------------------------------------------
-- Utilidades
---------------------------------------------------------------------------------------------------------------------

local function anchor() end

local function modDir()
    -- getinfo de uma função deste arquivo (não de nível: dentro de pcall o nível 1 seria o próprio pcall)
    local ok, info = pcall(debug.getinfo, anchor, "S")
    if ok and info and type(info.source) == "string" and info.source:sub(1, 1) == "@" then
        local p = info.source:sub(2):gsub("/", "\\")
        local d = p:match("^(.*)\\[Ss]cripts\\[^\\]+$")
        if d then return d end
    end
    return "ue4ss\\Mods\\cb-recon"
end

local MOD_DIR = modDir()
local LOG_FILE = MOD_DIR .. "\\cb-recon.log"
local STATE_FILE = MOD_DIR .. "\\estado.txt"

local function ts() return os.date("%Y-%m-%d %H:%M:%S") end

local function trunc(s)
    s = tostring(s)
    if #s <= CFG.maxStr then return s end
    -- corta numa fronteira de caractere UTF-8 (não deixa um byte de continuação 10xxxxxx no começo do resto)
    local cut = CFG.maxStr
    while cut > 0 do
        local b = s:byte(cut + 1)
        if not b or b < 0x80 or b >= 0xC0 then break end
        cut = cut - 1
    end
    return s:sub(1, cut) .. "...(+" .. (#s - cut) .. ")"
end

local warnedWrite = false
local function appendLine(path, line)
    local wrote = false
    pcall(function()
        local f = io.open(path, "a")
        if f then f:write(line, "\n"); f:close(); wrote = true end
    end)
    if not wrote and not warnedWrite then
        -- ex.: caminho com acento (io.open usa a página de código ANSI): sem log nem estado.txt nesta sessão
        warnedWrite = true
        pcall(print, "[cb-recon] AVISO: não consigo gravar em " .. tostring(path) .. " (sem permissão ou caminho com acentos?)\n")
    end
    return wrote
end

local function log(level, msg)
    local line = ts() .. " [" .. level .. "] " .. tostring(msg)
    pcall(print, "[cb-recon] " .. level .. ": " .. tostring(msg) .. "\n")
    appendLine(LOG_FILE, line)
end

local function info(m) log("INFO", m) end
local function warn(m) log("AVISO", m) end
local function err(m) log("ERRO", m) end

-- Roda fn protegida; devolve ok, resultado-ou-mensagem (com traceback).
local function safe(what, fn, ...)
    local args = table.pack(...)
    local ok, res = xpcall(function() return fn(table.unpack(args, 1, args.n)) end, function(e)
        return tostring(e) .. "\n" .. debug.traceback("", 2)
    end)
    if not ok then err(what .. " falhou: " .. trunc(res)) end
    return ok, res
end

-- Chamada protegida silenciosa (para leituras que podem falhar sem importância).
local function try(fn, ...)
    local ok, res = pcall(fn, ...)
    if ok then return res end
    return nil
end

local function valid(o)
    if o == nil then return false end
    local t = type(o)
    if t ~= "userdata" and t ~= "table" then return false end
    local ok, v = pcall(function() return o:IsValid() end)
    return ok and v == true
end

local function fullName(o)
    if not valid(o) then return nil end
    return try(function() return o:GetFullName() end)
end

local function shortName(o)
    if not valid(o) then return nil end
    return try(function() return o:GetFName():ToString() end)
end

local function address(o)
    if not valid(o) then return nil end
    return try(function() return o:GetAddress() end)
end

local function sameObject(a, b)
    local x, y = address(a), address(b)
    return x ~= nil and x == y
end

local function classOf(o)
    if not valid(o) then return nil end
    local c = try(function() return o:GetClass() end)
    if valid(c) then return c end
    return nil
end

-- Cadeia de classes: {"Class /Script/Phoenix.Biped_Player", ..., "Class /Script/CoreUObject.Object"}
local function classChain(o)
    local out = {}
    local c = classOf(o)
    local n = 0
    while valid(c) and n < 48 do
        out[#out + 1] = fullName(c) or "?"
        c = try(function() return c:GetSuperStruct() end)
        n = n + 1
    end
    return out
end

local function isEnginePath(full)
    if not full then return true end
    return full:find("/Script/Engine.", 1, true) ~= nil or full:find("/Script/CoreUObject.", 1, true) ~= nil
end

local function findClass(path)
    local c = try(function() return StaticFindObject(path) end)
    if valid(c) then return c end
    return nil
end

local function isA(o, cls)
    if not cls or not valid(o) then return false end
    return try(function() return o:IsA(cls) end) == true
end

---------------------------------------------------------------------------------------------------------------------
-- Formatação de valores lidos do jogo (limitada e defensiva)
---------------------------------------------------------------------------------------------------------------------

local STRUCT_FIELDS = {
    { "X", "Y", "Z" }, { "Pitch", "Yaw", "Roll" }, { "R", "G", "B", "A" }, { "X", "Y" },
}

local function fmtValue(v, depth)
    depth = depth or 0
    local t = type(v)
    if t == "nil" then return nil end
    if t == "boolean" then return v end
    if t == "number" then
        if v ~= v or v == math.huge or v == -math.huge then return tostring(v) end
        return v
    end
    if t == "string" then return trunc(v) end
    if t ~= "userdata" and t ~= "table" then return "<" .. t .. ">" end

    local kind = try(function() return v:type() end)
    kind = type(kind) == "string" and kind or ""

    if kind:find("FName", 1, true) or kind:find("FString", 1, true) or kind:find("FText", 1, true)
        or kind:find("String", 1, true) then
        local s = try(function() return v:ToString() end)
        if s ~= nil then return trunc(s) end
    end
    if kind == "TArray" then
        local n = try(function() return v:GetArrayNum() end) or try(function() return #v end)
        local out = { tipo = "TArray", n = n }
        if depth < 1 and n and n > 0 then
            out.primeiros = {}
            pcall(function()
                v:ForEach(function(i, elem)
                    if i > CFG.maxArrayPreview then return true end
                    local e = try(function() return elem:get() end)
                    out.primeiros[#out.primeiros + 1] = fmtValue(e, depth + 1)
                    return false
                end)
            end)
        end
        return out
    end
    if kind:find("Struct", 1, true) then
        for _, fields in ipairs(STRUCT_FIELDS) do
            local out, okAll = {}, true
            for _, f in ipairs(fields) do
                local fv = try(function() return v[f] end)
                if type(fv) ~= "number" then okAll = false; break end
                out[f] = fv
            end
            if okAll then return out end
        end
        local fn = try(function() return v:GetFullName() end)
        if fn then return trunc(fn) end
        return "<" .. kind .. ">"
    end
    -- UObject (ou derivado)
    local okValid, isValid = pcall(function() return v:IsValid() end)
    if okValid then
        if not isValid then return "<invalido>" end
        local fn = try(function() return v:GetFullName() end)
        if fn then return trunc(fn) end
    end
    local s = try(function() return v:ToString() end)
    if s ~= nil then return trunc(s) end
    return "<" .. (kind ~= "" and kind or t) .. ">"
end

-- Tipos de propriedade cujo valor lemos (os demais só aparecem com nome e tipo).
local READ_TYPES = {
    BoolProperty = true, ByteProperty = true, Int8Property = true, Int16Property = true, IntProperty = true,
    Int64Property = true, UInt16Property = true, UInt32Property = true, UInt64Property = true,
    FloatProperty = true, DoubleProperty = true, EnumProperty = true, NameProperty = true, StrProperty = true,
    TextProperty = true, ObjectProperty = true, ClassProperty = true, StructProperty = true, ArrayProperty = true,
}

-- Propriedades declaradas pelas classes do jogo (cadeia até a primeira classe do motor), com valores.
local function gameProperties(o, limit)
    limit = limit or CFG.maxPropsPerObject
    local out, count = {}, 0
    local c = classOf(o)
    local n = 0
    while valid(c) and n < 48 do
        local cfull = fullName(c)
        if isEnginePath(cfull) then break end
        local owner = shortName(c) or "?"
        pcall(function()
            c:ForEachProperty(function(prop)
                if count >= limit then return true end
                local name = try(function() return prop:GetFName():ToString() end)
                local ptype = try(function() return prop:GetClass():GetFName():ToString() end) or "?"
                if name then
                    local item = { nome = name, tipo = ptype, classe = owner }
                    if READ_TYPES[ptype] then
                        local okv, val = pcall(function() return o[name] end)
                        if okv then item.valor = fmtValue(val) else item.erro = trunc(val) end
                    end
                    out[#out + 1] = item
                    count = count + 1
                end
                return false
            end)
        end)
        if count >= limit then out[#out + 1] = { aviso = "limite de " .. limit .. " propriedades" }; break end
        c = try(function() return c:GetSuperStruct() end)
        n = n + 1
    end
    return out
end

-- Lê propriedades com nome conhecido (do motor), ignorando as que não existem.
local function namedProperties(o, names)
    local out = {}
    for _, name in ipairs(names) do
        local okv, val = pcall(function() return o[name] end)
        if okv and val ~= nil then out[name] = fmtValue(val) end
    end
    return out
end

---------------------------------------------------------------------------------------------------------------------
-- JSON (mínimo, chaves ordenadas)
---------------------------------------------------------------------------------------------------------------------

local function jsonEscape(s)
    s = s:gsub('[%c"\\]', function(c)
        if c == '"' then return '\\"' end
        if c == "\\" then return "\\\\" end
        if c == "\n" then return "\\n" end
        if c == "\r" then return "\\r" end
        if c == "\t" then return "\\t" end
        return string.format("\\u%04x", c:byte())
    end)
    return s
end

local function isArray(t)
    local n = 0
    for k in pairs(t) do
        if type(k) ~= "number" or k < 1 or k % 1 ~= 0 then return false end
        n = n + 1
    end
    for i = 1, n do if t[i] == nil then return false end end
    return true
end

local function toJson(v, indent, seen)
    indent = indent or ""
    seen = seen or {}
    local t = type(v)
    if v == nil then return "null" end
    if t == "boolean" then return v and "true" or "false" end
    if t == "number" then
        if v ~= v or v == math.huge or v == -math.huge then return '"' .. tostring(v) .. '"' end
        if math.type(v) == "integer" then return tostring(v) end
        return string.format("%.6g", v)
    end
    if t == "string" then return '"' .. jsonEscape(v) .. '"' end
    if t ~= "table" then return '"' .. jsonEscape(tostring(v)) .. '"' end
    if seen[v] then return '"<ciclo>"' end
    seen[v] = true
    local inner = indent .. "  "
    local parts = {}
    if isArray(v) then
        if #v == 0 then seen[v] = nil; return "[]" end
        for i = 1, #v do parts[#parts + 1] = inner .. toJson(v[i], inner, seen) end
        seen[v] = nil
        return "[\n" .. table.concat(parts, ",\n") .. "\n" .. indent .. "]"
    end
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = tostring(k) end
    table.sort(keys)
    if #keys == 0 then seen[v] = nil; return "{}" end
    for _, k in ipairs(keys) do
        local val = v[k]
        if val == nil then val = v[tonumber(k)] end
        parts[#parts + 1] = inner .. '"' .. jsonEscape(k) .. '": ' .. toJson(val, inner, seen)
    end
    seen[v] = nil
    return "{\n" .. table.concat(parts, ",\n") .. "\n" .. indent .. "}"
end

---------------------------------------------------------------------------------------------------------------------
-- Estado entre sessões (estado.txt: "<data> <hora> <etapa> <INICIADO|OK|ERRO|PULADO> [detalhe]")
---------------------------------------------------------------------------------------------------------------------

local function readState()
    local last = {}
    pcall(function()
        local f = io.open(STATE_FILE, "r")
        if not f then return end
        for line in f:lines() do
            local step, status = line:match("^%S+ %S+ (%S+) (%u+)")
            if step and status then last[step] = status end
        end
        f:close()
    end)
    return last
end

local function mark(step, status, detail)
    appendLine(STATE_FILE, ts() .. " " .. step .. " " .. status .. (detail and (" " .. trunc(detail):gsub("\n", " | ")) or ""))
end

---------------------------------------------------------------------------------------------------------------------
-- Encontrar o jogador local
---------------------------------------------------------------------------------------------------------------------

local function findLocalController()
    local list = try(function() return FindAllOf("PlayerController") end)
    if type(list) ~= "table" then return nil end
    local first
    for _, pc in ipairs(list) do
        if valid(pc) then
            first = first or pc
            local isLocal = try(function() return pc:IsLocalPlayerController() end)
            if isLocal == true then return pc end
        end
    end
    return first
end

local function worldOf(o)
    if not valid(o) then return nil end
    local w = try(function() return o:GetWorld() end)
    if valid(w) then return w end
    return nil
end

local function looksLikeMenu(worldName)
    if not worldName then return false end
    local low = worldName:lower()
    for _, h in ipairs(MENU_WORLD_HINTS) do
        if low:find(h, 1, true) then return true end
    end
    return false
end

---------------------------------------------------------------------------------------------------------------------
-- Relatório
---------------------------------------------------------------------------------------------------------------------

local function section(report, name, fn)
    local ok, res = safe("seção " .. name, fn)
    if ok then report[name] = res else report[name] = { erro = trunc(res) } end
end

local SKEL_PROPS = { "SkeletalMesh", "AnimClass", "AnimScriptInstance", "AnimationMode", "MasterPoseComponent",
    "LeaderPoseComponent", "bComponentUseFixedSkelBounds", "VisibilityBasedAnimTickOption", "bHiddenInGame" }
local STATIC_PROPS = { "StaticMesh", "bHiddenInGame" }
local SCENE_PROPS = { "AttachParent", "AttachSocketName", "RelativeLocation", "RelativeRotation", "RelativeScale3D",
    "bVisible" }
local MOVE_PROPS = { "MovementMode", "CustomMovementMode", "DefaultLandMovementMode", "Velocity", "MaxWalkSpeed",
    "MaxWalkSpeedCrouched", "MaxSwimSpeed", "MaxFlySpeed", "MaxCustomMovementSpeed", "MaxAcceleration",
    "BrakingDecelerationWalking", "BrakingDecelerationFalling", "GroundFriction", "GravityScale", "JumpZVelocity",
    "AirControl", "RotationRate", "bOrientRotationToMovement", "bUseControllerDesiredRotation",
    "NetworkSmoothingMode", "bRunPhysicsWithNoController", "MaxStepHeight", "WalkableFloorAngle", "Mass",
    "bEnablePhysicsInteraction", "bRequestedMoveUseAcceleration", "bUseRVOAvoidance" }
local PAWN_PROPS = { "bUseControllerRotationYaw", "bUseControllerRotationPitch", "AutoPossessPlayer", "AIControllerClass",
    "BaseEyeHeight", "Mesh", "CharacterMovement", "CapsuleComponent", "PlayerState", "bReplicates",
    "bReplicateMovement", "RootComponent", "Owner", "Instigator" }

local function describeComponent(comp, classes)
    local d = {
        nome = shortName(comp),
        classe = fullName(classOf(comp)),
        cadeia = classChain(comp),
    }
    if isA(comp, classes.scene) then d.cena = namedProperties(comp, SCENE_PROPS) end
    if isA(comp, classes.skel) then
        d.malha_esqueletal = namedProperties(comp, SKEL_PROPS)
        local anim = try(function() return comp:GetAnimInstance() end)
        if valid(anim) then
            d.anim_instance = { objeto = fullName(anim), cadeia = classChain(anim) }
            d.anim_instance.propriedades_jogo = gameProperties(anim, 80)
        end
    elseif isA(comp, classes.static) then
        d.malha_estatica = namedProperties(comp, STATIC_PROPS)
    end
    if not isEnginePath(d.classe) then d.propriedades_jogo = gameProperties(comp, 80) end
    return d
end

local function buildReport(pc, pawn, waitedSeconds)
    local report = {}
    local classes = {
        character = findClass("/Script/Engine.Character"),
        scene = findClass("/Script/Engine.SceneComponent"),
        skel = findClass("/Script/Engine.SkeletalMeshComponent"),
        static = findClass("/Script/Engine.StaticMeshComponent"),
    }

    section(report, "meta", function()
        local m = { mod = "cb-recon " .. VERSION, data = ts(), espera_segundos = waitedSeconds, lua = _VERSION }
        local okv, a, b, c = pcall(function() return UE4SS.GetVersion() end)
        if okv and a then m.ue4ss = string.format("%s.%s.%s", tostring(a), tostring(b), tostring(c)) end
        local maj = try(function() return UnrealVersion.GetMajor() end)
        local min = try(function() return UnrealVersion.GetMinor() end)
        if maj then m.unreal = tostring(maj) .. "." .. tostring(min) end
        return m
    end)

    section(report, "mundo", function()
        local w = worldOf(pawn) or worldOf(pc)
        local out = { mundo = fullName(w), nome = shortName(w) }
        if w then
            out.nivel_persistente = fullName(try(function() return w.PersistentLevel end))
            out.game_mode = { objeto = fullName(try(function() return w.AuthorityGameMode end)),
                cadeia = classChain(try(function() return w.AuthorityGameMode end)) }
            local gs = try(function() return w.GameState end)
            out.game_state = { objeto = fullName(gs), cadeia = classChain(gs) }
            local levels = try(function() return w.StreamingLevels end)
            if levels then
                out.niveis_streaming = { n = try(function() return levels:GetArrayNum() end), nomes = {} }
                pcall(function()
                    levels:ForEach(function(i, elem)
                        if i > CFG.maxStreamingLevels then return true end
                        local lv = try(function() return elem:get() end)
                        local item = { objeto = fullName(lv) }
                        local pkg = try(function() return lv.PackageNameToLoad end)
                        if pkg then item.pacote = fmtValue(pkg) end
                        local loaded = try(function() return lv:IsLevelLoaded() end)
                        if loaded ~= nil then item.carregado = loaded end
                        out.niveis_streaming.nomes[#out.niveis_streaming.nomes + 1] = item
                        return false
                    end)
                end)
            end
        end
        local gi = try(function() return FindFirstOf("GameInstance") end)
        out.game_instance = { objeto = fullName(gi), cadeia = classChain(gi) }
        local eng = try(function() return FindFirstOf("Engine") end)
        out.engine = fullName(eng)
        return out
    end)

    section(report, "controller", function()
        local out = { objeto = fullName(pc), cadeia = classChain(pc) }
        for _, prop in ipairs({ "PlayerCameraManager", "MyHUD", "PlayerState", "CheatManager", "PlayerInput" }) do
            local o = try(function() return pc[prop] end)
            if valid(o) then out[prop] = { objeto = fullName(o), cadeia = classChain(o) } end
        end
        out.propriedades_jogo = gameProperties(pc)
        return out
    end)

    section(report, "pawn", function()
        local out = { objeto = fullName(pawn), cadeia = classChain(pawn), e_character = isA(pawn, classes.character) }
        out.posicao = fmtValue(try(function() return pawn:K2_GetActorLocation() end))
        out.rotacao = fmtValue(try(function() return pawn:K2_GetActorRotation() end))
        out.propriedades_motor = namedProperties(pawn, PAWN_PROPS)
        local cap = try(function() return pawn.CapsuleComponent end)
        if valid(cap) then out.capsula = namedProperties(cap, { "CapsuleHalfHeight", "CapsuleRadius" }) end
        out.propriedades_jogo = gameProperties(pawn)
        return out
    end)

    section(report, "movimento", function()
        local mc = try(function() return pawn.CharacterMovement end)
        if not valid(mc) then return { aviso = "Pawn sem CharacterMovement" } end
        return {
            objeto = fullName(mc), cadeia = classChain(mc),
            propriedades_motor = namedProperties(mc, MOVE_PROPS),
            propriedades_jogo = gameProperties(mc),
        }
    end)

    section(report, "componentes", function()
        local out = { do_pawn = {}, do_controller = {} }
        local list = try(function() return FindAllOf("ActorComponent") end)
        if type(list) ~= "table" then return { aviso = "FindAllOf(ActorComponent) vazio" } end
        out.total_no_jogo = #list
        local pawnAddr, pcAddr = address(pawn), address(pc)
        for _, comp in ipairs(list) do
            local outer = try(function() return comp:GetOuter() end)
            local oa = address(outer)
            local bucket
            if oa ~= nil and oa == pawnAddr then bucket = out.do_pawn
            elseif oa ~= nil and oa == pcAddr then bucket = out.do_controller end
            if bucket then
                if #bucket >= CFG.maxComponents then
                    out.aviso = "limite de " .. CFG.maxComponents .. " componentes"
                else
                    local ok, d = safe("componente", describeComponent, comp, classes)
                    bucket[#bucket + 1] = ok and d or { erro = trunc(d) }
                end
            end
        end
        return out
    end)

    section(report, "atores_ligados", function()
        local out = {}
        local list = try(function() return FindAllOf("Actor") end)
        if type(list) ~= "table" then return { aviso = "FindAllOf(Actor) vazio" } end
        local total = #list
        for _, a in ipairs(list) do
            if #out >= CFG.maxOwnedActors then break end
            local owner = try(function() return a.Owner end)
            local parent = try(function() return a:GetAttachParentActor() end)
            local rel
            if sameObject(owner, pawn) then rel = "Owner=Pawn"
            elseif sameObject(parent, pawn) then rel = "anexado ao Pawn"
            elseif sameObject(owner, pc) then rel = "Owner=Controller" end
            if rel then
                out[#out + 1] = { objeto = fullName(a), relacao = rel, cadeia = classChain(a) }
            end
        end
        return { total_atores = total, ligados = out }
    end)

    section(report, "classes_por_padrao", function()
        local out = { padroes = {}, contagem = {} }
        for _, p in ipairs(PATTERNS) do out.padroes[p] = {}; out.contagem[p] = 0 end
        local list = try(function() return FindAllOf("Class") end)
        local scanned = 0
        local function consider(cls)
            scanned = scanned + 1
            local name = shortName(cls)
            if not name then return end
            for _, p in ipairs(PATTERNS) do
                if name:find(p, 1, true) then
                    out.contagem[p] = out.contagem[p] + 1
                    local bucket = out.padroes[p]
                    if #bucket < CFG.maxPerPattern then bucket[#bucket + 1] = fullName(cls) end
                end
            end
        end
        if type(list) == "table" then
            out.fonte = "FindAllOf(Class)"
            for _, cls in ipairs(list) do
                if scanned >= CFG.maxClassesScanned then out.aviso = "limite de classes"; break end
                consider(cls)
            end
        else
            out.fonte = "ForEachUObject"
            pcall(function()
                ForEachUObject(function(obj)
                    if scanned >= CFG.maxClassesScanned then return end
                    if try(function() return obj:IsClass() end) == true then consider(obj) end
                end)
            end)
        end
        out.classes_vistas = scanned
        out.limite_por_padrao = CFG.maxPerPattern
        return out
    end)

    return report
end

local reportSeq = 0
local function writeReport(report)
    reportSeq = reportSeq + 1
    local name = MOD_DIR .. "\\relatorio-" .. os.date("%Y%m%d-%H%M%S") .. "-" .. reportSeq .. ".json"
    local ok, e = pcall(function()
        local f = assert(io.open(name, "w"))
        f:write(toJson(report), "\n")
        f:close()
    end)
    if ok then info("relatório gravado: " .. name) else err("não consegui gravar o relatório (" .. trunc(e) .. ")") end
    return ok
end

---------------------------------------------------------------------------------------------------------------------
-- Agendamento (sempre no game thread quando a API existir)
---------------------------------------------------------------------------------------------------------------------

local function later(ms, fn)
    if type(ExecuteInGameThreadWithDelay) == "function" then
        return ExecuteInGameThreadWithDelay(ms, function() safe("tarefa", fn) end)
    end
    ExecuteWithDelay(ms, function()
        ExecuteInGameThread(function() safe("tarefa", fn) end)
    end)
end

local function loop(ms, fn) -- fn devolve true para parar
    if type(LoopInGameThreadWithDelay) == "function" and type(CancelDelayedAction) == "function" then
        local handle
        local stopped = false
        handle = LoopInGameThreadWithDelay(ms, function()
            if stopped then return end
            local ok, done = safe("laço", fn)
            if not ok or done then
                stopped = true
                pcall(CancelDelayedAction, handle)
            end
        end)
        return
    end
    LoopAsync(ms, function()
        local finished = false
        ExecuteInGameThread(function()
            local ok, done = safe("laço", fn)
            if not ok or done then finished = true end
        end)
        return finished
    end)
end

---------------------------------------------------------------------------------------------------------------------
-- Fluxo
---------------------------------------------------------------------------------------------------------------------

local RAN = false
local START = os.time()

local function finish(summary)
    mark("sessao", "FIM", summary)
    info("==================================================================")
    info("TUDO PRONTO (" .. summary .. "). Pode fazer o teste do F8 e depois sair do jogo.")
    info("==================================================================")
end

local function runDumps(prev, idx, results)
    if idx > #DUMP_STEPS then
        finish(table.concat(results, ", "))
        return
    end
    local step = DUMP_STEPS[idx]
    local last = prev[step.id]
    local function nextStep(res)
        results[#results + 1] = step.id .. "=" .. res
        later(CFG.gapMs, function() runDumps(prev, idx + 1, results) end)
    end
    if last == "OK" then
        info("etapa " .. step.id .. ": já feita numa sessão anterior, pulando")
        return nextStep("ja_feita")
    end
    if last == "INICIADO" then
        warn("etapa " .. step.id .. ": começou numa sessão anterior e não terminou (jogo fechou/travou?); pulando")
        mark(step.id, "PULADO", "travou ou fechou numa sessão anterior")
        return nextStep("pulada")
    end
    if last == "PULADO" or last == "ERRO" then
        info("etapa " .. step.id .. ": " .. last .. " numa sessão anterior; não repete")
        return nextStep(last:lower())
    end
    local f = rawget(_G, step.fn)
    if type(f) ~= "function" then
        warn("etapa " .. step.id .. ": função " .. step.fn .. " não existe nesta versão do UE4SS")
        mark(step.id, "ERRO", "função " .. step.fn .. " ausente")
        return nextStep("ausente")
    end
    info(string.format("etapa %d/%d: %s (%s) começando; o jogo pode ficar parado/\"não respondendo\" por alguns minutos",
        idx, #DUMP_STEPS, step.id, step.fn))
    mark(step.id, "INICIADO")
    local t0 = os.clock()
    local ok, res = safe(step.fn, f)
    local secs = string.format("%.1f s", os.clock() - t0)
    if ok then
        mark(step.id, "OK", secs)
        info("etapa " .. step.id .. " concluída em " .. secs .. " -> " .. step.saida)
        nextStep("ok")
    else
        mark(step.id, "ERRO", res)
        nextStep("erro")
    end
end

local function startRecon(pc, pawn, waited, prev)
    info("jogador encontrado depois de " .. waited .. " s: " .. tostring(fullName(pawn)))
    if prev.relatorio == "INICIADO" or prev.relatorio == "PULADO" then
        -- PULADO também não tenta de novo (senão alternaria travar/pular a cada sessão). ERRO (erro Lua, sem travar
        -- o jogo) tenta de novo.
        warn("o relatório travou numa sessão anterior; pulando o relatório e seguindo para os dumpers")
        if prev.relatorio == "INICIADO" then mark("relatorio", "PULADO", "travou numa sessão anterior") end
    else
        mark("relatorio", "INICIADO")
        local ok, report = safe("relatório", buildReport, pc, pawn, waited)
        if ok and writeReport(report) then mark("relatorio", "OK") else mark("relatorio", "ERRO", tostring(report)) end
    end
    later(CFG.gapMs, function() runDumps(prev, 1, {}) end)
end

-- Relatório extra (sem dumpers) quando o mundo ou o Pawn mudam depois do primeiro: cobre o caso de o primeiro ter
-- sido feito numa tela de menu que não reconhecemos. No máximo MAX_EXTRA por sessão.
local MAX_EXTRA = 3

local function main()
    if RAN then return end
    RAN = true
    info("cb-recon " .. VERSION .. " carregado; pasta " .. MOD_DIR)
    local prev = readState()
    mark("sessao", "INICIADO")
    info("aguardando o jogador entrar no mundo (carregue um save). Compilação de shaders, vídeos e menu não contam" ..
        " no limite de espera")

    local phase = "esperando"            -- esperando -> vigiando
    local candidate, since, lastStatus = nil, nil, 0
    local reportedKey, extras = nil, 0
    local cachedPc, tick, watchStart = nil, 0, nil
    local clockStart, timedOut = nil, false  -- relógio do limite: só a partir do 1º mundo fora de menu
    loop(CFG.pollMs, function()
        local now = os.time()
        local waited = now - START
        tick = tick + 1
        if phase == "vigiando" then
            -- vigia leve: a cada ~16 s, por no máximo 40 min
            if now - watchStart > 40 * 60 then info("fim da vigia de mudança de mundo"); return true end
            if tick % 8 ~= 0 then return false end
        end
        -- FindAllOf varre todos os objetos: só quando o controller guardado deixou de valer (ou a cada ~30 s, caso o
        -- jogo troque de controller sem destruir o anterior)
        if not valid(cachedPc) or tick % 15 == 0 then cachedPc = findLocalController() end
        local pc = cachedPc
        local pawn = pc and try(function() return pc.Pawn end)
        local charClass = findClass("/Script/Engine.Character")
        local w = worldOf(pawn) or worldOf(pc)
        local wname = shortName(w)
        local good = valid(pawn) and isA(pawn, charClass) and not looksLikeMenu(wname)
        local key = good and (tostring(wname) .. "#" .. tostring(address(pawn))) or nil

        if good then
            if candidate ~= key then
                candidate, since = key, now
                if phase == "esperando" or key ~= reportedKey then
                    info("candidato: " .. tostring(fullName(pawn)) .. " no mundo " .. tostring(wname) ..
                        "; esperando " .. CFG.stableSeconds .. " s de estabilidade")
                end
            elseif now - since >= CFG.stableSeconds and key ~= reportedKey then
                reportedKey = key
                if phase == "esperando" then
                    phase, watchStart = "vigiando", now
                    startRecon(pc, pawn, waited, prev)
                else
                    extras = extras + 1
                    info("mundo/Pawn mudou (" .. tostring(wname) .. "); relatório extra " .. extras .. "/" .. MAX_EXTRA)
                    local ok, report = safe("relatório extra", buildReport, pc, pawn, waited)
                    if ok then writeReport(report) end
                    if extras >= MAX_EXTRA then info("limite de relatórios extras; parando de vigiar"); return true end
                end
            end
        else
            candidate, since = nil, nil
        end

        if phase == "esperando" then
            if not clockStart and valid(pc) and wname and not looksLikeMenu(wname) then
                clockStart = now
                info("primeiro mundo fora de menu (" .. tostring(wname) .. "); limite de espera: " ..
                    (CFG.timeoutSeconds // 60) .. " min a partir de agora")
            end
            if now - lastStatus >= CFG.statusEverySeconds then
                lastStatus = now
                info(string.format("aguardando (%d s desde o início, %s no mundo): controller=%s pawn=%s mundo=%s",
                    waited, clockStart and ((now - clockStart) .. " s") or "ainda não", tostring(shortName(pc)),
                    tostring(shortName(pawn)), tostring(wname)))
            end
            if clockStart and not timedOut and now - clockStart >= CFG.timeoutSeconds then
                -- Não desiste: grava o que achou e continua esperando. O guia manda reabrir o jogo se nada mudar;
                -- TIMEOUT não é etapa, então a próxima sessão tenta tudo de novo.
                timedOut = true
                err("tempo esgotado esperando o jogador (" .. (CFG.timeoutSeconds // 60) .. " min no mundo). " ..
                    "Gravei um relatório parcial e continuo esperando; se nada acontecer, saia, abra o jogo de novo " ..
                    "e carregue o save logo")
                mark("sessao", "TIMEOUT")
                if valid(pc) then
                    local ok, report = safe("relatório parcial", buildReport, pc, pawn, waited)
                    if ok then writeReport(report) end
                end
            end
        end
        return false
    end)
end

local ok, e = pcall(main)
if not ok then
    pcall(print, "[cb-recon] ERRO ao iniciar: " .. tostring(e) .. "\n")
    appendLine(LOG_FILE, ts() .. " [ERRO] ao iniciar: " .. tostring(e))
end
