#include "Reflect.hpp"

#include <cbmp/client/IGame.hpp>

#include <Helpers/String.hpp>
#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/FStrProperty.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UPackage.hpp>
#include <Unreal/UnrealFlags.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace cbmp::client {
namespace {
// Só no game thread: nenhum cache tem trava.
struct FunctionLayout {
    std::vector<FProperty*> params;  // todas as propriedades da função (parâmetros e, em Blueprint, locais)
    FProperty* returnProp = nullptr;
    size_t bufferSize = 0;  // maior offset + tamanho
};

std::map<std::pair<const void*, StringType>, UFunction*>& FunctionCache() {
    static std::map<std::pair<const void*, StringType>, UFunction*> cache;
    return cache;
}
std::map<const void*, FunctionLayout>& LayoutCache() {
    static std::map<const void*, FunctionLayout> cache;
    return cache;
}
std::map<std::pair<const void*, StringType>, FProperty*>& PropertyCache() {
    static std::map<std::pair<const void*, StringType>, FProperty*> cache;
    return cache;
}
std::map<StringType, UClass*>& ClassCache() {
    static std::map<StringType, UClass*> cache;
    return cache;
}

StringType Key(const TCHAR* name) { return StringType(ToCharTypePtr(name)); }

std::string Utf8(const StringType& s) {
    std::string out;
    out.reserve(s.size());
    for (auto c : s) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');  // nomes do motor: ASCII
    return out;
}

const FunctionLayout& LayoutOf(UFunction* fn) {
    auto& cache = LayoutCache();
    if (const auto it = cache.find(fn); it != cache.end()) return it->second;
    FunctionLayout layout;
    for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated)) {
        layout.params.push_back(p);
        const int32 end = p->GetOffset_Internal() + p->GetSize();
        if (end > 0) layout.bufferSize = std::max(layout.bufferSize, static_cast<size_t>(end));
        if (p->HasAnyPropertyFlags(CPF_ReturnParm)) layout.returnProp = p;
    }
    return cache.emplace(fn, std::move(layout)).first->second;
}

FProperty* FindParam(const FunctionLayout& layout, const TCHAR* name) {
    const StringType key = Key(name);
    for (FProperty* p : layout.params) {
        if (p->GetName() == key) return p;
    }
    return nullptr;
}

bool IsStructNamed(FProperty* p, const CharType* structName) {
    if (!p->IsA<FStructProperty>()) return false;
    UScriptStruct* s = static_cast<FStructProperty*>(p)->GetStruct().Get();
    return s && s->GetName() == structName;
}

// Strings construídas no buffer: destruídas no fim da chamada (mesmo se o motor lançar SEH).
struct OwnedStrings {
    std::vector<FString*> items;
    ~OwnedStrings() {
        for (FString* s : items) std::destroy_at(s);
    }
};

[[noreturn]] void BadArg(const TCHAR* function, const TCHAR* arg, const char* why) {
    throw GameError(Utf8(Key(function)) + ": parâmetro " + Utf8(Key(arg)) + " " + why);
}

void WriteArg(const TCHAR* function, const Arg& arg, FProperty* p, uint8* buffer, OwnedStrings& owned) {
    void* dst = buffer + p->GetOffset_Internal();
    const auto size = static_cast<size_t>(p->GetSize());
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                if (!p->IsA<FBoolProperty>()) BadArg(function, arg.name, "não é bool");
                static_cast<FBoolProperty*>(p)->SetPropertyValueInContainer(buffer, v);
            } else if constexpr (std::is_same_v<T, int32_t>) {
                if (!p->IsA<FIntProperty>() || size != sizeof(v)) BadArg(function, arg.name, "não é int32");
                std::memcpy(dst, &v, sizeof(v));
            } else if constexpr (std::is_same_v<T, float>) {
                if (!p->IsA<FFloatProperty>() || size != sizeof(v)) BadArg(function, arg.name, "não é float");
                std::memcpy(dst, &v, sizeof(v));
            } else if constexpr (std::is_same_v<T, double>) {
                if (!p->IsA<FDoubleProperty>() || size != sizeof(v)) BadArg(function, arg.name, "não é double");
                std::memcpy(dst, &v, sizeof(v));
            } else if constexpr (std::is_same_v<T, FVector>) {
                // FVector do UE4SS guarda floats (UE4) ou doubles (UE5) no começo: copia o tamanho do motor.
                if (!IsStructNamed(p, STR("Vector")) || size > sizeof(v)) BadArg(function, arg.name, "não é Vector");
                std::memcpy(dst, &v, size);
            } else if constexpr (std::is_same_v<T, FRotator>) {
                if (!IsStructNamed(p, STR("Rotator")) || size > sizeof(v)) BadArg(function, arg.name, "não é Rotator");
                std::memcpy(dst, &v, size);
            } else if constexpr (std::is_same_v<T, UObject*>) {
                if (!p->IsA<FObjectProperty>() || size != sizeof(v)) BadArg(function, arg.name, "não é objeto");
                std::memcpy(dst, &v, sizeof(v));
            } else if constexpr (std::is_same_v<T, StringType>) {
                if (!p->IsA<FStrProperty>() || size != sizeof(FString)) BadArg(function, arg.name, "não é FString");
                owned.items.push_back(new (dst) FString(v));
            }
        },
        arg.value);
}
}  // namespace

UFunction* FindFunction(UObject* self, const TCHAR* function) {
    if (!self) throw GameError(Utf8(Key(function)) + ": objeto nulo");
    UClass* cls = self->GetClassPrivate();
    if (!cls) throw GameError(Utf8(Key(function)) + ": objeto sem classe");
    auto& cache = FunctionCache();
    auto key = std::make_pair(static_cast<const void*>(cls), Key(function));
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    UFunction* fn = self->GetFunctionByNameInChain(function);
    cache.emplace(std::move(key), fn);
    return fn;
}

std::optional<StringType> FirstStringParam(UFunction* fn) {
    if (!fn) return std::nullopt;
    for (FProperty* p : LayoutOf(fn).params) {
        if (p->IsA<FStrProperty>() && !p->HasAnyPropertyFlags(CPF_ReturnParm)) return p->GetName();
    }
    return std::nullopt;
}

bool CallByName(UObject* self, const TCHAR* function, std::initializer_list<Arg> args, void* returnOut,
                size_t returnSize) {
    UFunction* fn = FindFunction(self, function);
    if (!fn) return false;
    const FunctionLayout& layout = LayoutOf(fn);

    std::vector<uint8> buffer(layout.bufferSize, 0);  // operator new: alinhado a 16 bytes
    OwnedStrings owned;
    for (const Arg& arg : args) {
        FProperty* p = FindParam(layout, arg.name);
        if (!p || p == layout.returnProp) BadArg(function, arg.name, "não existe");
        WriteArg(function, arg, p, buffer.data(), owned);
    }
    if (returnOut && !layout.returnProp) throw GameError(Utf8(Key(function)) + ": sem valor de retorno");

    self->ProcessEvent(fn, buffer.empty() ? nullptr : buffer.data());

    if (returnOut) {
        FProperty* rp = layout.returnProp;
        if (rp->IsA<FBoolProperty>() && returnSize == sizeof(bool)) {
            const bool value = static_cast<FBoolProperty*>(rp)->GetPropertyValueInContainer(buffer.data());
            std::memcpy(returnOut, &value, sizeof(value));
        } else {
            const auto size = static_cast<size_t>(rp->GetSize());
            if (size > returnSize) throw GameError(Utf8(Key(function)) + ": retorno maior que o esperado");
            std::memcpy(returnOut, buffer.data() + rp->GetOffset_Internal(), size);
        }
    }
    return true;
}

std::optional<bool> CallReturningBool(UObject* self, const TCHAR* function, std::initializer_list<Arg> args) {
    UFunction* fn = FindFunction(self, function);
    if (!fn) return std::nullopt;
    FProperty* rp = LayoutOf(fn).returnProp;
    if (!rp || !rp->IsA<FBoolProperty>()) throw GameError(Utf8(Key(function)) + ": retorno não é bool");
    bool value = false;
    CallByName(self, function, args, &value, sizeof(value));
    return value;
}

std::optional<UObject*> CallReturningObject(UObject* self, const TCHAR* function, std::initializer_list<Arg> args) {
    UFunction* fn = FindFunction(self, function);
    if (!fn) return std::nullopt;
    FProperty* rp = LayoutOf(fn).returnProp;
    if (!rp || !rp->IsA<FObjectProperty>() || rp->GetSize() != sizeof(UObject*)) {
        throw GameError(Utf8(Key(function)) + ": retorno não é objeto");
    }
    UObject* value = nullptr;
    CallByName(self, function, args, &value, sizeof(value));
    return value;
}

namespace {
FProperty* FindProperty(UObject* self, const TCHAR* name) {
    if (!self) throw GameError(Utf8(Key(name)) + ": objeto nulo");
    UClass* cls = self->GetClassPrivate();
    if (!cls) throw GameError(Utf8(Key(name)) + ": objeto sem classe");
    auto& cache = PropertyCache();
    auto key = std::make_pair(static_cast<const void*>(cls), Key(name));
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    FProperty* found = nullptr;
    for (FProperty* p : TFieldRange<FProperty>(cls, EFieldIterationFlags::Default)) {
        if (p->GetName() == key.second) {
            found = p;
            break;
        }
    }
    cache.emplace(std::move(key), found);
    return found;
}

[[noreturn]] void BadProp(const TCHAR* name, const char* why) {
    throw GameError("propriedade " + Utf8(Key(name)) + " " + why);
}

template <class T>
std::optional<T> ReadStructProp(UObject* self, const TCHAR* name, const CharType* structName) {
    FProperty* p = FindProperty(self, name);
    if (!p) return std::nullopt;
    const auto size = static_cast<size_t>(p->GetSize());
    if (!IsStructNamed(p, structName) || size > sizeof(T)) BadProp(name, "com tipo inesperado");
    T value{};
    std::memcpy(&value, p->ContainerPtrToValuePtr<void>(self), size);
    return value;
}
}  // namespace

std::optional<UObject*> ReadObjectProp(UObject* self, const TCHAR* name) {
    FProperty* p = FindProperty(self, name);
    if (!p) return std::nullopt;
    if (!p->IsA<FObjectProperty>() || p->GetSize() != sizeof(UObject*)) BadProp(name, "não é objeto");
    UObject* value = nullptr;
    std::memcpy(&value, p->ContainerPtrToValuePtr<void>(self), sizeof(value));
    return value;
}

std::optional<FVector> ReadVectorProp(UObject* self, const TCHAR* name) {
    return ReadStructProp<FVector>(self, name, STR("Vector"));
}

std::optional<FRotator> ReadRotatorProp(UObject* self, const TCHAR* name) {
    return ReadStructProp<FRotator>(self, name, STR("Rotator"));
}

std::optional<uint8_t> ReadByteProp(UObject* self, const TCHAR* name) {
    FProperty* p = FindProperty(self, name);
    if (!p) return std::nullopt;
    if (!(p->IsA<FByteProperty>() || p->IsA<FEnumProperty>()) || p->GetSize() != 1) BadProp(name, "não é byte");
    uint8_t value = 0;
    std::memcpy(&value, p->ContainerPtrToValuePtr<void>(self), 1);
    return value;
}

bool WriteVectorProp(UObject* self, const TCHAR* name, const FVector& v) {
    FProperty* p = FindProperty(self, name);
    if (!p) return false;
    const auto size = static_cast<size_t>(p->GetSize());
    if (!IsStructNamed(p, STR("Vector")) || size > sizeof(v)) BadProp(name, "não é Vector");
    std::memcpy(p->ContainerPtrToValuePtr<void>(self), &v, size);
    return true;
}

UClass* FindClassByName(const TCHAR* shortName) {
    auto& cache = ClassCache();
    StringType key = Key(shortName);
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    UClass* cls = UObjectGlobals::FindObject<UClass>(UObjectGlobals::ANY_PACKAGE, shortName);
    cache.emplace(std::move(key), cls);
    return cls;
}

UObject* DefaultObjectOf(const TCHAR* shortName) {
    UClass* cls = FindClassByName(shortName);
    if (!cls) return nullptr;
    return cls->GetClassDefaultObject().Get();
}
}  // namespace cbmp::client
