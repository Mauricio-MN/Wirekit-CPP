# Guia da API

Este guia descreve a API pública de `wirekit/wirekit.h` e o contrato da biblioteca. O container DI continua experimental; consulte a seção [Estado e compatibilidade](#estado-e-compatibilidade) antes de integrá-lo.

## Instalação e compilação

A biblioteca é header-only. Inclua `wirekit/wirekit.h` no projeto e compile em C++17 ou superior. Para instalar e consumir via CMake, use `cmake --install build --prefix <prefixo>` e `find_package(wirekit CONFIG REQUIRED)`; os alvos exportados são `wirekit::wirekit` e `wirekit::single_thread`.

```cpp
#include <wirekit/wirekit.h>
```

Exemplo de verificacao do header:

```sh
printf '#include <wirekit/wirekit.h>\nint main() { return 0; }\n' \
  | g++ -std=c++17 -pthread -Isrc/include -x c++ -fsyntax-only -
```

## Conceitos

- **Serviço (`T`)**: tipo da interface ou classe base cujas instâncias serão guardadas pelo hub.
- **Canal (`wirekit::Channel`)**: identificador nomeado que agrupa instâncias. Canais com o mesmo nome compartilham o grupo no mesmo hub.
- **Hub (`wirekit::Hub<T>`)**: mantém instâncias de `T` por canal, cria instâncias por meio de uma fábrica virtual e despacha chamadas de método para todas as instâncias do canal.
- **Fábrica**: uma classe derivada pode sobrescrever `createFor` e escolher uma implementação concreta a partir do tipo de quem solicitou o serviço.

`wirekit::ChannelPtr` é o handle compartilhado e opaco do grupo de um Hub.

## Channel

`wirekit::Channel` é um valor que contém apenas o nome do canal. Não é uma instância de serviço nem um escopo DI hierárquico. `wirekit::ChannelPtr` aponta para um handle distinto, criado pelo Hub e associado ao estado do grupo. O dispatch pelo handle carrega o snapshot atomicamente, sem busca no mapa nem mutex do registro; os overloads que recebem `Channel&` continuam resolvendo pelo nome.

`wirekit::Channel` aceita um nome opcional. O construtor sem argumentos usa `"Default"`.

```cpp
wirekit::Channel defaultChannel;
wirekit::Channel requestChannel("request-42");
wirekit::Hub<int> hub;
auto requestHandle = hub.registerChannel(requestChannel);
```

`getName()` retorna o identificador do canal. `registerChannel(name)` cria o canal quando necessário e retorna o canal já registrado quando o nome existe.

`findChannel(name)` apenas consulta o registro. Retorna um `wirekit::ChannelPtr` nulo quando não encontra o nome e nunca cria um canal. Use essa função para recuperação; reserve `registerChannel` para criação ou reutilização. Um handle só pode ser usado no Hub que o criou; passar um handle estrangeiro a uma operação lança `std::invalid_argument`. Após `erase` ou `clear`, handles antigos continuam válidos como objetos, mas ficam inativos; registrar o mesmo nome cria um handle novo.

## Criacao do hub

O construtor sem argumentos é recomendado. O construtor que recebe um inteiro permanece por compatibilidade e não tem efeito. O hub deve permanecer vivo enquanto for usado; a coleta de canais vazios é explícita via `collectGarbage()`. Handles `wirekit::ChannelPtr` mantêm o canal vivo, mas não estendem a vida do hub.

```cpp
wirekit::Hub<IMyService> hub;
```

Para uma aplicação estritamente single-thread, defina `WIREKIT_SINGLE_THREAD` em todas as unidades de tradução que incluem o header. Isso remove mutexes reais; o hub deixa de ser seguro para uso concorrente. No GCC, por exemplo:

```sh
g++ -std=c++17 -DWIREKIT_SINGLE_THREAD -Isrc/include \
  src/tests/hub_test_st.cpp -o /tmp/hub_test_st
/tmp/hub_test_st
```

A macro deve ser uniforme no programa inteiro: mistura entre unidades de tradução com configurações diferentes pode causar incompatibilidade de ODR/ABI. Nesse modo, assim como no modo padrão, chame `collectGarbage()` para remover canais vazios sem handles externos.

Para interfaces abstratas, sobrescreva a fábrica antes de solicitar instâncias: a fábrica padrão tenta construir `T` diretamente.

## Adicionar instâncias

As sobrecargas de `addInstance` aceitam um canal por referência ou por `wirekit::ChannelPtr`. Para escolher a fábrica por um tipo solicitante, prefira a forma parametrizada:

```cpp
auto channel = hub.addInstance<StaffRequest>(requestChannel);
hub.addInstance<NormalRequest>(channel);
```

`StaffRequest` e `NormalRequest` são apenas tipos usados como tags. Nenhum objeto precisa ser criado ou mantido vivo. A sobrecarga `addInstance(channel, caller)` também existe para selecionar pela classe de um objeto que já existe; esse objeto só é consultado durante a chamada e não é retido pelo hub.

As outras formas disponíveis são:

- `addInstance(channel, caller)` escolhe pela classe de um objeto existente, usado apenas durante a chamada.
- `addInstance(channel, object)` copia o objeto; requer que `T` seja concreto e copiável.
- `addInstance(channel, shared_ptr)` compartilha uma instância existente, recomendado para interfaces abstratas e para evitar cópia ou slicing.

Cada chamada adiciona uma instância ao grupo do canal; não é uma operação de registro singleton. O valor retornado é o `wirekit::ChannelPtr` para endereçar o grupo. O hub mantém grupos com instâncias registradas até `erase` ou `clear`; descartar o handle não apaga os serviços. `collectGarbage()` remove canais vazios sem handles externos. `Channel&` continua sendo um identificador por nome e pode ser usado com hubs diferentes; `ChannelPtr` é específico do Hub que o criou.

Os objetos de serviço são mantidos por `std::shared_ptr` dentro do hub. Não há `new` nem ponteiro bruto na API do exemplo. Chame `removeInstance`, `unsubscribe`, `erase` ou `clear` para liberar as referências do hub; handles externos e snapshots de chamadas em andamento podem manter os objetos vivos.

## Despachar métodos

`callMethod(channel, method, args...)` chama o método em cada instância do canal. `callMethodGet(channel, method, args...)` faz o mesmo e devolve os resultados em um `std::vector`, preservando a ordem de registro das instâncias.

Exemplo de chamada, supondo um canal que já tenha instâncias:

```cpp
hub.callMethod(channel, &IMyService::reset, 7);
auto results = hub.callMethodGet(channel, &IMyService::value);
```

O método deve ser invocável em `T` com os argumentos informados. O tipo de retorno de `callMethodGet` é deduzido do método. Um canal sem instâncias é um no-op; `callMethodGet` retorna um vetor vazio. Um `ChannelPtr` nulo lança `std::invalid_argument`. Se um callback lança, a exceção se propaga e as instâncias seguintes não são chamadas. Os argumentos são recebidos por valor e reutilizados em cada callback; métodos que recebem parâmetros por valor podem causar cópias por instância.

`removeInstance(channel, sharedPtr)` remove uma ocorrência da instância correspondente naquele canal e retorna `true`; retorna `false` se a instância não estiver registrada. Para serviços criados pela fábrica, use `subscribe`, que retorna `Hub<T>::Subscription`, e depois `unsubscribe(token)`. Os overloads de `subscribe` aceitam as mesmas formas de registro de `addInstance`. Cada token identifica uma única inscrição; usá-lo novamente retorna `false`. Snapshots de despachos já iniciados mantêm suas referências até terminar.

## Fábrica de implementações

A fábrica virtual oferece a assinatura histórica e uma variante que aceita `const std::type_info&`; a variante com `const` delega para a histórica por compatibilidade:

```cpp
virtual std::shared_ptr<T> createFor(std::type_info& requester);
virtual std::shared_ptr<T> createFor(const std::type_info& requester);
```

Uma implementação derivada pode comparar `requester` com `typeid(CallerType)` e retornar uma instância concreta convertível para `std::shared_ptr<T>`. Os tipos concretos devem implementar `T` e ser destruídos corretamente por meio da interface, que deve ter destrutor virtual quando for polimórfica.

Para reduzir o boilerplate da classe derivada, `wirekit/wirekit.h` define a macro `WIREKIT_DECLARE_FACTORY(Nome, Interface)`. Ela declara a subclasse e o override da fábrica; a política de seleção continua implementada pelo usuário, de preferência no `.cpp`:

```cpp
namespace app {
WIREKIT_DECLARE_FACTORY(UserService, IUser);
}
```

```cpp
std::shared_ptr<app::IUser> app::UserService::createFor(std::type_info& requester) {
  if (requester == typeid(StaffRequest)) {
    return std::make_shared<UserStaff>();
  }
  throw std::invalid_argument("Unsupported requester");
}
```

A macro não cria implementações automaticamente; ela apenas padroniza a declaração do hub e mantém a fábrica fora do header.

## Remover canais

`erase(channel)` e `erase(channelId)` removem do hub as instâncias e o canal associados ao identificador. `clear()` remove todos os registros e pode ser chamado mais de uma vez. `collectGarbage()` remove somente canais sem instâncias e cujo último handle externo já foi liberado; está disponível nos dois modos de sincronização.

A mesma instância `shared_ptr<T>` pode ser registrada em mais de um canal. Apagar um canal libera apenas a referência daquele grupo; a instância continua viva enquanto outro canal ou o chamador mantiver uma referência. `clear()` remove todas as referências mantidas pelo hub, mas não invalida `shared_ptr`s externos. Compartilhar uma instância não causa comportamento indefinido por si só; chamadas concorrentes a métodos mutáveis dessa mesma instância precisam de sincronização implementada pelo próprio serviço.

## Concorrência e desempenho

O modo padrão é multithread: operações do registro e `collectGarbage()` são protegidos por mutex. Não existe macro `WIREKIT_MULTI_THREAD`; não definir `WIREKIT_SINGLE_THREAD` seleciona o modo padrão. Cada despacho pelo handle canônico captura o snapshot imutável do grupo sem buscar o nome no mapa nem adquirir o mutex do registro. A biblioteca padrão não garante que operações atômicas sobre `shared_ptr` sejam lock-free. Assim, callbacks podem chamar novamente o hub; se uma instância ou canal for removido durante uma chamada, as instâncias do snapshot permanecem vivas até o despacho terminar. `WIREKIT_SINGLE_THREAD` mantém os snapshots e essa semântica de vida, mas substitui os mutexes por um lock no-op. Não use essa configuração se qualquer operação do hub puder ocorrer concorrentemente.

Adicionar uma instância cria um novo snapshot, com custo proporcional ao número de instâncias daquele canal. O despacho pelo handle compartilha o snapshot em tempo constante, sem copiar o vetor nem fazer busca por nome. O benchmark comparativo pode ser compilado e executado assim:

```sh
g++ -std=c++17 -O2 -pthread -Isrc/include src/benchmarks/dispatch_benchmark.cpp -o /tmp/wirekit_dispatch_benchmark
/tmp/wirekit_dispatch_benchmark
g++ -std=c++17 -O2 -DWIREKIT_SINGLE_THREAD -Isrc/include src/benchmarks/dispatch_benchmark.cpp -o /tmp/wirekit_dispatch_single
/tmp/wirekit_dispatch_single
```

Os resultados são dependentes do hardware e servem para comparar estratégias neste ambiente, não como garantia de latência. O modo single-thread não adquire mutexes reais.

`collectGarbage()` percorre o registro sob mutex e deve ser chamado em um ponto adequado à aplicação. Não há thread de coleta automática. O benchmark de registro mede a contenção de buscas concorrentes enquanto o hub é usado; ele não representa uma coleta periódica em segundo plano:

```sh
g++ -std=c++17 -O2 -pthread -Isrc/include src/benchmarks/collector_benchmark.cpp -o /tmp/wirekit_collector_benchmark
/tmp/wirekit_collector_benchmark
```

Métodos de `T` continuam concorrentes e podem ser chamados simultaneamente. A sincronização do estado interno do serviço é responsabilidade de `T`. O último `shared_ptr` pode ser liberado pela thread chamadora ou durante `clear()`/destruição; serviços com afinidade de thread devem controlar a destruição explicitamente. A destruição do hub não pode concorrer com chamadas públicas.

Os itens planejados e limites conhecidos estão em [`../ROADMAP.md`](../ROADMAP.md).

## Exemplo completo

O arquivo [`src/examples/hub/hub_example.cpp`](../src/examples/hub/hub_example.cpp) é um roteiro comentado para acompanhar o fluxo. As partes estão separadas em `src/examples/hub/`:

- `iuser.h` declara a interface compartilhada.
- `user_staff.h/.cpp` e `user_normal.h/.cpp` declaram e implementam as classes concretas.
- `request_types.h` declara tipos vazios usados para identificar cada solicitante.
- `user_service.h/.cpp` implementa a fábrica; este é o único componente que inclui as classes concretas.
- `src/examples/hub/hub_example.cpp` cria o hub, registra as duas implementações no mesmo canal, demonstra o despacho e também usa o overload baseado em objeto caller.
- `registration_modes.cpp` demonstra as sobrecargas que copiam um objeto concreto ou compartilham um `std::shared_ptr<T>`.

Depois do primeiro despacho, o exemplo também mostra como passar `UserService` por referência para uma função, recuperar um canal pelo nome e criar um segundo canal independente no mesmo hub. Ele usa `addInstance<TipoSolicitante>` para evitar instâncias de tags que precisariam ficar no escopo.

Os corpos das implementações ficam em arquivos `.cpp`, para que alterar a lógica de uma classe concreta não exija recompilar os outros arquivos-fonte. Alterações em headers compartilhados ainda exigem recompilar seus dependentes.

Compile e execute na raiz do repositorio:

```sh
g++ -std=c++17 -pthread -Isrc/include \
  src/examples/hub/hub_example.cpp \
  src/examples/hub/user_service.cpp \
  src/examples/hub/user_staff.cpp \
  src/examples/hub/user_normal.cpp \
  -o /tmp/wirekit_example
/tmp/wirekit_example
```

Saída esperada:

```text
Staff ID: 21
Normal ID: 42
Access through a function: 21, 42
Found named channel: 21, 42
Missing channel was not created by lookup.
Independent channel: 14
After caller leaves scope: 13
```

O caller usado em `addInstance(channel, caller)` não é armazenado; o exemplo o destrói antes de despachar métodos e comprova que o serviço continua disponível.

Para executar o exemplo de copia versus ownership compartilhado:

```sh
g++ -std=c++17 -pthread -Isrc/include src/examples/hub/registration_modes.cpp -o /tmp/wirekit_registration_modes
/tmp/wirekit_registration_modes
```

Ele imprime `Hub copy: 4`, `Original object: 3` e `Shared object: 11`: a primeira sobrecarga copia o valor, enquanto a segunda compartilha a instância fornecida.

## Estado e compatibilidade

### DI experimental

Com `WIREKIT_ENABLE_DI` definido antes de `#include <wirekit/wirekit.h>`, o header `wirekit/container.h` é incluído e expõe `wirekit::Container`. As dependências são deduzidas do construtor: cada parâmetro `std::shared_ptr<T>` (ou `const std::shared_ptr<T>&`) é resolvido no container. Se houver vários construtores viáveis, vale o que tem mais parâmetros injetáveis (até 8); parâmetros de outros tipos não são suportados.

```cpp
struct Logger : ILogger { explicit Logger(std::shared_ptr<IClock>); };
struct Handler { Handler(std::shared_ptr<ILogger>, std::shared_ptr<IClock>); };

wirekit::Container container;
container.bind<IClock, SystemClock>();                           // singleton
container.bind<ILogger, Logger>(wirekit::Lifetime::Transient);   // IClock injetado
auto handler = container.create<Handler>();                        // consumidor não registrado
```

`bind<I, Impl, Deps...>()` com lista explícita ainda é aceito, e `bindFactory`/`bindInstance` cobrem fábricas manuais e instâncias prontas. `resolve` lança `std::out_of_range` sem binding e `std::logic_error` em dependência circular. Fábricas executam sem o mutex do registro, então podem resolver outras dependências. Um singleton é criado exatamente uma vez, mesmo com várias threads pedindo ao mesmo tempo: a criação é serializada por um mutex recursivo próprio, e quem chega depois recebe a instância pronta. Consequência: uma fábrica de singleton não deve bloquear esperando outra thread que também resolva um singleton. Fábricas de `Transient` rodam em paralelo. Destrutores dos serviços (ao substituir um binding, em `clear()` ou na destruição do container) rodam fora dos mutexes, mas o último `shared_ptr` pode ser liberado por qualquer thread, e a destruição do container não pode concorrer com `resolve`/`create`; um destrutor também não deve chamar `resolve`. A macro só adiciona uma classe, então não muda o layout de `Hub`. A API é experimental e pode mudar.

O container DI é experimental. A validação antecipada de bindings e o suporte a construtores com mais de oito dependências estão no [`roadmap`](../ROADMAP.md). A criação automática considera parâmetros `std::shared_ptr` e atualmente examina até oito parâmetros.
