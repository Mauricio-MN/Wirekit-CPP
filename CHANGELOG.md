# Changelog

## 0.1.0

- Biblioteca header-only em C++17 com `wirekit::Hub<T>` e container DI opcional.
- Hub multicast por canal com snapshots imutáveis, ordem de registro preservada e callbacks fora do mutex.
- Adicionados tokens `subscribe`/`unsubscribe`, `removeInstance`, despacho no-op para canais sem instâncias e coleta explícita via `collectGarbage()`.
- `Channel` permanece como valor nominal; `ChannelPtr` passa a ser um handle opaco vinculado ao Hub, com snapshot atômico para dispatch direto. Handles estrangeiros são rejeitados e handles removidos ficam inativos.
- Removido o coletor em thread por Hub; a coleta de canais vazios passou a ser manual nos modos multithread e single-thread. `WIREKIT_SINGLE_THREAD` continua opt-in; o modo padrão continua multithread.
- O diagnóstico de construtores DI agora informa o limite de oito dependências injetadas automaticamente.
- Adicionados alvos CMake instaláveis e exportados, além de CI com ASan, UBSan e TSan.
- Histórico de benchmark da implementação anterior à mudança de estado do canal: 33 ns por despacho com 1 instância, 37 ns com 8, 135 ns com 64 e 448 ns com 256. Esses valores não representam a implementação atual e variam conforme o hardware.