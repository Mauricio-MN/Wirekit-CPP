# Guia da API

Este guia descreve a API publica de `wirekit.h` e o contrato pretendido da biblioteca. O codigo ainda esta em desenvolvimento; consulte a secao [Estado e compatibilidade](#estado-e-compatibilidade) antes de integrar.

## Instalacao e compilacao

A biblioteca e header-only. Inclua `wirekit.h` no projeto e compile em C++17 ou superior. Por padrao, o hub usa mutexes e um coletor em thread separada; em GCC/Linux, compile e vincule com `-pthread`.

```cpp
#include "wirekit.h"
```

Exemplo de verificacao do header:

```sh
printf '#include "wirekit.h"\nint main() { return 0; }\n' \
  | g++ -std=c++17 -pthread -Isrc/include -x c++ -fsyntax-only -
```

## Conceitos

- **Servico (`T`)**: tipo da interface ou classe base cujas instancias serao guardadas pelo hub.
- **Canal (`wirekit::Channel`)**: identificador nomeado que agrupa instancias. Canais com o mesmo nome sao destinados a compartilhar o mesmo grupo no mesmo hub.
- **Hub (`wirekit::Hub<T>`)**: mantem instancias de `T` por canal, cria instancias por meio de uma fabrica virtual e despacha chamadas de metodo para todas as instancias do canal.
- **Fabrica**: uma classe derivada pode sobrescrever `createFor` e escolher uma implementacao concreta a partir do tipo de quem solicitou o servico.

`wirekit::ChannelPtr` e o handle compartilhado de um canal.

## Channel

`wirekit::Channel` representa a chave nomeada de um grupo de instancias dentro de um hub. Nao e uma instancia de servico nem um escopo DI hierarquico. `wirekit::ChannelPtr` e o handle compartilhado desse grupo.

`wirekit::Channel` aceita um nome opcional. O construtor sem argumentos usa `"Default"`.

```cpp
wirekit::Channel defaultChannel;
wirekit::Channel requestChannel("request-42");
auto sharedChannel = std::make_shared<wirekit::Channel>("request-42");
```

`getName()` retorna o identificador do canal. `registerChannel(name)` cria o canal quando necessario e retorna o canal ja registrado quando o nome existe.

`findChannel(name)` apenas consulta o registro. Retorna um `wirekit::ChannelPtr` nulo quando nao encontra o nome e nunca cria um canal. Use essa funcao para recuperacao; reserve `registerChannel` para criacao-ou-reutilizacao.

## Criacao do hub

O construtor sem argumentos usa o intervalo padrao do coletor de canais. O construtor que recebe um inteiro configura esse intervalo em milissegundos. O hub deve permanecer vivo enquanto for usado; sua destruicao interrompe e aguarda o coletor. Handles `wirekit::ChannelPtr` mantem o canal vivo, mas nao estendem a vida do hub.

```cpp
wirekit::Hub<IMyService> hub;
```

Para uma aplicacao estritamente single-thread, defina `WIREKIT_SINGLE_THREAD` em todas as unidades de traducao que incluem o header. Isso remove mutexes reais e a thread do coletor; o hub deixa de ser seguro para uso concorrente. No GCC, por exemplo:

```sh
g++ -std=c++17 -DWIREKIT_SINGLE_THREAD -Isrc/include \
  src/tests/hub_test_st.cpp -o /tmp/hub_test_st
/tmp/hub_test_st
```

A macro deve ser uniforme no programa inteiro: mistura entre unidades de traducao com configuracoes diferentes pode causar incompatibilidade de ODR/ABI. Nesse modo, o argumento de intervalo do construtor e ignorado e a coleta automatica nao e iniciada.

Para interfaces abstratas, sobrescreva a fabrica antes de solicitar instancias: a fabrica padrao tenta construir `T` diretamente.

## Adicionar instancias

As sobrecargas de `addInstance` aceitam um canal por referencia ou por `wirekit::ChannelPtr`. Para escolher a fabrica por um tipo solicitante, prefira a forma parametrizada:

```cpp
auto channel = hub.addInstance<StaffRequest>(requestChannel);
hub.addInstance<NormalRequest>(channel);
```

`StaffRequest` e `NormalRequest` sao apenas tipos usados como tags. Nenhum objeto precisa ser criado ou mantido vivo. A sobrecarga `addInstance(channel, caller)` tambem existe para selecionar pela classe de um objeto que ja existe; esse objeto so e consultado durante a chamada e nao e retido pelo hub.

As outras formas disponiveis sao:

- `addInstance(channel, caller)` escolhe pela classe de um objeto existente, usado apenas durante a chamada.
- `addInstance(channel, object)` copia o objeto; requer que `T` seja concreto e copiavel.
- `addInstance(channel, shared_ptr)` compartilha uma instancia existente, recomendado para interfaces abstratas e para evitar copia ou slicing.

Cada chamada adiciona uma instancia ao grupo do canal; nao e uma operacao de registro singleton. O valor retornado e o `wirekit::ChannelPtr` para enderecar o grupo. O hub mantem grupos com instancias registradas ate `erase` ou `clear`; descartar o handle nao apaga os servicos. O coletor automatico remove somente canais sem instancias e sem handles externos.

Os objetos de servico sao mantidos por `std::shared_ptr` dentro do hub. Nao ha `new` nem ponteiro bruto na API do exemplo. Chame `erase` ou `clear` para liberar as referencias do hub; handles externos e snapshots de chamadas em andamento podem manter os objetos vivos.

## Despachar metodos

`callMethod(channel, method, args...)` chama o metodo em cada instancia do canal. `callMethodGet(channel, method, args...)` faz o mesmo e devolve os resultados em um `std::vector`, preservando a ordem de registro das instancias.

Exemplo de chamada, supondo um canal que ja tenha instancias:

```cpp
hub.callMethod(channel, &IMyService::reset, 7);
auto results = hub.callMethodGet(channel, &IMyService::value);
```

O metodo deve ser invocavel em `T` com os argumentos informados. O tipo de retorno de `callMethodGet` e deduzido do metodo. Um canal sem instancias lanca `std::out_of_range`; um `ChannelPtr` nulo lanca `std::invalid_argument`. Se um callback lanca, a excecao se propaga e as instancias seguintes nao sao chamadas. Os argumentos sao recebidos por valor e reutilizados em cada callback; metodos que recebem parametros por valor podem causar copias por instancia.

## Fabrica de implementacoes

A fabrica virtual oferece a assinatura historica e uma variante que aceita `const std::type_info&`; a variante com `const` delega para a historica por compatibilidade:

```cpp
virtual std::shared_ptr<T> createFor(std::type_info& requester);
virtual std::shared_ptr<T> createFor(const std::type_info& requester);
```

Uma implementacao derivada pode comparar `requester` com `typeid(CallerType)` e retornar uma instancia concreta convertivel para `std::shared_ptr<T>`. Os tipos concretos devem implementar `T` e ser destruidos corretamente por meio da interface, que deve ter destrutor virtual quando for polimorfica.

Para reduzir o boilerplate da classe derivada, `wirekit.h` define a macro `WIREKIT_DECLARE_FACTORY(Nome, Interface)`. Ela declara a subclasse e o override da fabrica; a politica de selecao continua implementada pelo usuario, de preferencia no `.cpp`:

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

A macro nao cria implementacoes automaticamente; ela apenas padroniza a declaracao do hub e mantem a fabrica fora do header.

## Remover canais

`erase(channel)` e `erase(channelId)` removem do hub as instancias e o canal associados ao identificador. `clear()` remove todos os registros e pode ser chamado mais de uma vez. No modo multithread, o destrutor para o coletor e chama `clear()` antes de destruir o estado interno do hub. No modo single-thread, use `collectGarbage()` para remover os canais cujo ultimo handle externo ja foi liberado; `clear()` continua disponivel para remover tudo imediatamente.

A mesma instancia `shared_ptr<T>` pode ser registrada em mais de um canal. Apagar um canal libera apenas a referencia daquele grupo; a instancia continua viva enquanto outro canal ou o chamador mantiver uma referencia. `clear()` remove todas as referencias mantidas pelo hub, mas nao invalida `shared_ptr`s externos. Compartilhar uma instancia nao causa comportamento indefinido por si so; chamadas concorrentes a metodos mutaveis dessa mesma instancia precisam de sincronizacao implementada pelo proprio servico.

## Concorrencia e desempenho

No modo multithread, as alteracoes do registro sao protegidas por mutex. Cada despacho captura um snapshot imutavel compartilhado e executa os metodos sem manter o mutex do hub. Assim, callbacks podem chamar novamente o hub; se um canal for removido durante uma chamada, as instancias do snapshot permanecem vivas ate o despacho terminar. `WIREKIT_SINGLE_THREAD` mantem os snapshots e essa semantica de vida durante o despacho, mas substitui os mutexes por um lock no-op e desativa o coletor automatico. Nao use essa configuracao se qualquer operacao do hub puder ocorrer concorrentemente.

Adicionar uma instancia cria um novo snapshot, com custo proporcional ao numero de instancias daquele canal. O despacho compartilha o snapshot em tempo constante, sem copiar o vetor. O benchmark comparativo pode ser compilado e executado assim:

```sh
g++ -std=c++17 -O2 -pthread -Isrc/include src/benchmarks/dispatch_benchmark.cpp -o /tmp/wirekit_dispatch_benchmark
/tmp/wirekit_dispatch_benchmark
g++ -std=c++17 -O2 -DWIREKIT_SINGLE_THREAD -Isrc/include src/benchmarks/dispatch_benchmark.cpp -o /tmp/wirekit_dispatch_single
/tmp/wirekit_dispatch_single
```

Os resultados sao dependentes do hardware e servem para comparar estrategias neste ambiente, nao como garantia de latencia. As execucoes locais variaram bastante e nao estabeleceram um ganho estavel de latencia por despacho. O beneficio garantido do modo single-thread e estrutural: nao cria a thread do coletor e nao adquire mutexes reais.

O coletor examina no maximo quatro canais por trecho exclusivo do mutex e faz uma pausa curta entre lotes. Isso reduz o tempo de trabalho continuo sob o lock, mas nao estabelece um limite de latencia real: o escalonador, a concorrencia e o tamanho total do mapa afetam cada varredura. `garbageWait` e um intervalo de espera, nao um prazo maximo para coleta. O benchmark de contenção pode comparar coleta frequente com uma coleta praticamente inativa:

```sh
g++ -std=c++17 -O2 -pthread -Isrc/include src/benchmarks/collector_benchmark.cpp -o /tmp/wirekit_collector_benchmark
/tmp/wirekit_collector_benchmark 1
/tmp/wirekit_collector_benchmark 2147483647
```

Metodos de `T` continuam concorrentes e podem ser chamados simultaneamente. A sincronizacao do estado interno do servico e responsabilidade de `T`. O ultimo `shared_ptr` pode ser liberado pela thread do coletor, por uma thread chamadora, ou durante `clear()`/destruicao; servicos com afinidade de thread devem controlar a destruicao explicitamente. A destruicao do hub nao pode concorrer com chamadas publicas.

O plano para testar e reduzir pausas do coletor, definir garantias de concorrencia e tratar servicos com afinidade de thread esta em [`../ROADMAP.md`](../ROADMAP.md), Fase 3. Essas melhorias ainda nao foram implementadas.

## Exemplo completo

O arquivo [`src/examples/hub/hub_example.cpp`](../src/examples/hub/hub_example.cpp) e um roteiro comentado para acompanhar o fluxo. As partes estao separadas em `src/examples/hub/`:

- `iuser.h` declara a interface compartilhada.
- `user_staff.h/.cpp` e `user_normal.h/.cpp` declaram e implementam as classes concretas.
- `request_types.h` declara tipos vazios usados para identificar cada solicitante.
- `user_service.h/.cpp` implementa a fabrica; este e o unico componente que inclui as classes concretas.
- `src/examples/hub/hub_example.cpp` cria o hub, registra as duas implementacoes no mesmo canal, demonstra o despacho e tambem usa o overload baseado em objeto caller.
- `registration_modes.cpp` demonstra as sobrecargas que copiam um objeto concreto ou compartilham um `std::shared_ptr<T>`.

Depois do primeiro despacho, o exemplo tambem mostra como passar `UserService` por referencia para uma funcao, recuperar um canal pelo nome e criar um segundo canal independente no mesmo hub. Ele usa `addInstance<TipoSolicitante>` para evitar instancias de tags que precisariam ficar no escopo.

Os corpos das implementacoes ficam em arquivos `.cpp`, para que alterar a logica de uma classe concreta nao exija recompilar os outros arquivos-fonte. Alteracoes em headers compartilhados ainda exigem recompilar seus dependentes.

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

Saida esperada:

```text
Staff ID: 21
Normal ID: 42
Access through a function: 21, 42
Found named channel: 21, 42
Missing channel was not created by lookup.
Independent channel: 14
After caller leaves scope: 13
```

O caller usado em `addInstance(channel, caller)` nao e armazenado; o exemplo o destroi antes de despachar metodos e comprova que o servico continua disponivel.

Para executar o exemplo de copia versus ownership compartilhado:

```sh
g++ -std=c++17 -pthread -Isrc/include src/examples/hub/registration_modes.cpp -o /tmp/wirekit_registration_modes
/tmp/wirekit_registration_modes
```

Ele imprime `Hub copy: 4`, `Original object: 3` e `Shared object: 11`: a primeira sobrecarga copia o valor, enquanto a segunda compartilha a instancia fornecida.

## Estado e compatibilidade

### DI experimental

Com `WIREKIT_ENABLE_DI` definido antes de `#include "wirekit.h"`, o header `wirekit/container.h` e incluido e expoe `wirekit::Container`. As dependencias sao deduzidas do construtor: cada parametro `std::shared_ptr<T>` (ou `const std::shared_ptr<T>&`) e resolvido no container. Se houver varios construtores viaveis, vale o que tem mais parametros injetaveis (ate 8); parametros de outros tipos nao sao suportados.

```cpp
struct Logger : ILogger { explicit Logger(std::shared_ptr<IClock>); };
struct Handler { Handler(std::shared_ptr<ILogger>, std::shared_ptr<IClock>); };

wirekit::Container container;
container.bind<IClock, SystemClock>();                           // singleton
container.bind<ILogger, Logger>(wirekit::Lifetime::Transient);   // IClock injetado
auto handler = container.create<Handler>();                        // consumidor nao registrado
```

`bind<I, Impl, Deps...>()` com lista explicita ainda e aceito, e `bindFactory`/`bindInstance` cobrem fabricas manuais e instancias prontas. `resolve` lanca `std::out_of_range` sem binding e `std::logic_error` em dependencia circular. Fabricas executam sem o mutex do registro, entao podem resolver outras dependencias. Um singleton e criado exatamente uma vez, mesmo com varias threads pedindo ao mesmo tempo: a criacao e serializada por um mutex recursivo proprio, e quem chega depois recebe a instancia pronta. Consequencia: uma fabrica de singleton nao deve bloquear esperando outra thread que tambem resolva um singleton. Fabricas de `Transient` rodam em paralelo. Destrutores dos servicos (ao substituir um binding, em `clear()` ou na destruicao do container) rodam fora dos mutexes, mas o ultimo `shared_ptr` pode ser liberado por qualquer thread, e a destruicao do container nao pode concorrer com `resolve`/`create`; um destrutor tambem nao deve chamar `resolve`. A macro so adiciona uma classe, entao nao muda o layout de `Hub`. A API e experimental e pode mudar.

O header original nao compila em C++17. Os planos de trabalho e o resultado de sua execucao estao registrados em [`../ROADMAP.md`](../ROADMAP.md). Ate que a compilacao e os testes passem, as descricoes acima registram o contrato pretendido, nao uma garantia da implementacao.
