# Roadmap

Este documento lista trabalho futuro. Mudanças já entregues e resultados de validação ficam em [CHANGELOG.md](CHANGELOG.md).

## Hub

- [x] Usar `ChannelPtr` como handle opaco vinculado a um Hub; rejeitar handles estrangeiros e manter handles obsoletos inativos.
- [ ] Avaliar uma fábrica `std::function` injetável como alternativa à subclasse e às sobrecargas históricas de `createFor`.
- [ ] Avaliar uma estrutura persistente em blocos para buscar inserção O(1) amortizada sem invalidar snapshots imutáveis; medir a troca entre custo de inscrição e despacho.
- [ ] Definir um contrato de encaminhamento de argumentos que suporte tipos move-only e multicast com mais de um assinante.

## Container DI

- [ ] Adicionar validação do grafo de dependências antes da primeira criação e mensagens que identifiquem bindings ausentes.
- [ ] Avaliar elevar ou remover o limite de oito dependências automaticamente injetáveis; excedê-lo já é erro de compilação e a mensagem identifica esse limite.
- [ ] Corrigir a publicação de singletons quando um `bind` substitui a binding durante um `resolve` em andamento.

## Qualidade

- [ ] Avaliar uma biblioteca de testes que forneça descoberta e relatórios melhores que `assert` em `main`.
- [ ] Medir novamente os benchmarks de despacho e contenção em ambiente controlado após a alteração do estado do canal.
