#pragma once
// Única forma de chamar o jogo e de ler/gravar propriedades: por NOME, resolvido pela reflexão do motor (UFunction e
// FProperty). Nenhum offset fixo: o tamanho do buffer de parâmetros, a posição de cada parâmetro e a do retorno vêm das
// propriedades da função. Cada nome é resolvido uma vez por classe VIVA e guardado (achado ou ausente): a chave é o
// índice + série da classe no GUObjectArray, nunca o endereço, então uma classe descarregada e outra no mesmo lugar
// não herdam o cache (ver Reflect.cpp).
// Só no game thread (os caches não têm trava). Só parâmetros (CPF_Parm) são aceitos como argumento; os não passados
// ficam com bytes zero, por isso FText e contêineres (TArray, TMap, TSet) que não forem passados recusam a chamada
// (GameError, antes do ProcessEvent). Não serve para FString de saída: o buffer é liberado sem destruir o que o motor
// gravou nele.
#include <Unreal/Rotator.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <String/StringType.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <variant>

namespace RC::Unreal {
class UObject;
class UClass;
class UFunction;
class FProperty;
}  // namespace RC::Unreal

// Literal de nome para as APIs do motor (TCHAR pode ser outro tipo de 16 bits que o CharType do STR()).
#define CBMP_T(s) ::RC::FromCharTypePtr<::RC::Unreal::TCHAR>(STR(s))

namespace cbmp::client {
using ArgValue = std::variant<bool, int32_t, float, double, RC::Unreal::FVector, RC::Unreal::FRotator,
                              RC::Unreal::UObject*, RC::StringType>;
struct Arg {
    const RC::Unreal::TCHAR* name;
    ArgValue value;
};

// false = a função não existe nesta classe (nada é chamado). Lança GameError se um argumento pedido não existe na
// função ou tem tipo inesperado, se `returnOut` foi pedido e a função não tem retorno ou ele não cabe em `returnSize`.
// Retorno bool (FBoolProperty) com returnSize == sizeof(bool) é lido como bool; os demais, copiados byte a byte.
bool CallByName(RC::Unreal::UObject* self, const RC::Unreal::TCHAR* function, std::initializer_list<Arg> args,
                void* returnOut = nullptr, size_t returnSize = 0);

// Conveniências sobre CallByName para os retornos usados pela camada do jogo. nullopt = função ausente.
std::optional<bool> CallReturningBool(RC::Unreal::UObject* self, const RC::Unreal::TCHAR* function,
                                      std::initializer_list<Arg> args = {});
std::optional<RC::Unreal::UObject*> CallReturningObject(RC::Unreal::UObject* self, const RC::Unreal::TCHAR* function,
                                                        std::initializer_list<Arg> args = {});

// UFunction da classe de `self` (e das mães) pelo nome; nullptr se não existe. Guardada por (índice + série da classe,
// nome).
RC::Unreal::UFunction* FindFunction(RC::Unreal::UObject* self, const RC::Unreal::TCHAR* function);

// Nome do primeiro parâmetro de texto (FString) da função; nullopt se não há.
std::optional<RC::StringType> FirstStringParam(RC::Unreal::UFunction* fn);

// Propriedades do objeto pelo nome (na classe e nas mães), guardadas por (índice + série da classe, nome). Todas devolvem nullopt se a
// propriedade não existe; lançam GameError se ela existe com tipo diferente do pedido.
std::optional<RC::Unreal::UObject*> ReadObjectProp(RC::Unreal::UObject* self, const RC::Unreal::TCHAR* name);
std::optional<RC::Unreal::FVector> ReadVectorProp(RC::Unreal::UObject* self, const RC::Unreal::TCHAR* name);
std::optional<RC::Unreal::FRotator> ReadRotatorProp(RC::Unreal::UObject* self, const RC::Unreal::TCHAR* name);
std::optional<uint8_t> ReadByteProp(RC::Unreal::UObject* self, const RC::Unreal::TCHAR* name);  // byte ou enum de 1 byte
bool WriteVectorProp(RC::Unreal::UObject* self, const RC::Unreal::TCHAR* name, const RC::Unreal::FVector& v);

// UClass pelo nome curto ("Biped_Player", "BP_PlayerPerceptionPoint_C"); nullptr se não carregada. Guardada como
// referência fraca (descarregada → procurada de novo); a ausência só evita nova procura por 5 s (classes Blueprint
// podem carregar depois).
RC::Unreal::UClass* FindClassByName(const RC::Unreal::TCHAR* shortName);

// Objeto padrão da classe (para chamar funções estáticas); nullptr se a classe não existe.
RC::Unreal::UObject* DefaultObjectOf(const RC::Unreal::TCHAR* shortName);
}  // namespace cbmp::client
