# Indicação de status e botão de reset

Placa: ESP32-C6-DevKitM-1. O LED RGB (WS2812) fica no GPIO8 e o botão **BOOT** no GPIO9.

## LED de status

O LED é atualizado continuamente a partir do estado da rede e das impressoras.
As prioridades vão de cima para baixo: a primeira condição verdadeira define a cor.

| Cor / padrão | Significado | O que fazer |
|---|---|---|
| **Branco fixo** (só no início) | Firmware inicializando | Aguardar 1 a 2 s. |
| **Magenta piscando rápido** | Botão BOOT sendo segurado; contagem para reset de fábrica em andamento | Soltar para cancelar. Segurar 5 s para apagar tudo. |
| **Branco fixo** (após magenta) | Reset de fábrica disparado; reinicia em ~1,5 s | Nada. Ao voltar, o LED ficará azul. |
| **Ciano piscando rápido** | Trabalho de impressão em curso: dados sendo enviados à impressora, ou aguardando a confirmação dela | Aguardar. Via raw dura o envio mais ~1 s; via IPP ou LPD pode durar até 15 s a mais, esperando a resposta da impressora. |
| **Azul respirando** (lento) | Sem rede WiFi configurada. O AP de configuração está ativo | Conectar ao AP `PrintService-XXXX` com a senha impressa no serial e configurar o WiFi. |
| **Laranja piscando rápido** | WiFi configurado, tentando conectar. O AP continua ativo | Aguardar. Se persistir, verificar SSID e senha. |
| **Vermelho piscando** | Conectado à rede, mas ao menos uma impressora cadastrada ficou 3 sondagens seguidas sem responder ao SNMP | Verificar se a impressora está ligada, na mesma rede e com SNMP habilitado; se o IP estiver errado, remova e inclua de novo. |
| **Laranja fixo** | Conectado, todas respondem, mas alguma impressora tem alerta | Ver a aba **Impressoras** no portal: pouco toner, papel, tampa, atolamento etc. |
| **Verde respirando** (lento) | Conectado à rede, nenhuma impressora cadastrada | Incluir impressoras pelo portal (busca mDNS ou IP) ou pelo menu serial. |
| **Verde fixo** (fraco) | Conectado, todas as impressoras online e sem alertas | Nada. Estado normal. |

Notas:

- Toda impressora monitorada foi incluída pelo operador (busca mDNS ou IP); a busca sozinha não cadastra nada. O vermelho só acende após 3 sondagens seguidas sem resposta, cerca de 90 s a partir da inclusão ou da queda; antes disso a impressora conta como "ainda não sondada" e não altera a cor.
- O AP de configuração continua ligado por 2 min após o WiFi conectar (`AP=1 STA=1` no serial). Isso não muda a cor: com WiFi conectado, o LED reflete só as impressoras e a impressão.
- O LED **não** indica o estado da ligação com o servidor externo (WebSocket). Esse estado aparece no portal, aba Dispositivo, e na opção 1 do menu serial, com o motivo em caso de falha (DNS, TCP, TLS ou recusa HTTP).
- Alerta de suprimento: toner ou cilindro com 10% ou menos, reservatório de resíduo com 90% ou mais, ou qualquer erro em `hrPrinterDetectedErrorState` (sem papel, tampa aberta, atolamento, offline, manutenção).
- O AP de configuração desliga sozinho 2 min após o WiFi estabilizar, se ninguém estiver conectado a ele. Ele volta se o WiFi ficar 60 s fora.
- O brilho é limitado por software (cerca de 12% do máximo) para não ofuscar.

## Botão BOOT: reset de fábrica

1. Com a placa ligada, segure o botão **BOOT**.
2. Após 0,3 s o LED passa a piscar em magenta e o serial mostra a contagem a cada segundo.
3. Solte antes de 5 s para cancelar. O LED volta ao estado normal.
4. Ao atingir 5 s, o LED fica branco, as configurações são apagadas e a placa reinicia em modo AP.

O que é apagado: rede WiFi, senha do AP (volta à padrão derivada do MAC), senha do portal, community SNMP (volta a `public`), todas as impressoras cadastradas e a URL/token do servidor externo.

O que não muda: o nome do dispositivo, pois ele é sempre derivado do MAC.

Observação: segurar BOOT **durante a energização** coloca o chip em modo de gravação de firmware. O reset de fábrica só vale com o firmware já em execução.

## Identidade do dispositivo

| Item | Origem | Exemplo |
|---|---|---|
| Nome / hostname / SSID do AP | Últimos 2 bytes do MAC | `PrintService-3A7F` |
| Senha padrão do AP | Hash FNV-1a do MAC completo, 8 dígitos hex | `9C41E0B7` |
| Endereço mDNS na rede | nome + `.local` | `http://PrintService-3A7F.local/` |

A senha padrão do AP aparece no serial no boot (linha `[AP] ativo`) e na opção **1) Mostrar status** do menu. Ela pode ser trocada pelo portal ou pelo menu serial; o reset de fábrica a restaura.

## Menu serial

Conecte a 115200 bps, digite `menu` e Enter.

| Opção | Ação |
|---|---|
| 1 | Mostrar status: rede, AP e sua senha, portal, servidor externo (estado e motivo de falha) e último trabalho de impressão |
| 2 | Configurar rede WiFi (SSID e senha). Reinicia em seguida |
| 3 | Senha do AP de configuração (`-` deixa o AP aberto) |
| 4 | Senha do portal web, usuário `admin` (`-` remove) |
| 5 | Buscar redes WiFi |
| 6 | Reiniciar |
| 7 | Restaurar padrões de fábrica (confirmar com `SIM`) |
| 8 | Listar impressoras cadastradas e suprimentos |
| f | Buscar impressoras na rede por mDNS (~9 s) e listar nome, IP e portas; não inclui nada sozinha |
| a | Incluir impressora por IP |
| d | Remover impressora |
| c | Community SNMP |
| r | Forçar sondagem SNMP imediata |
| t | Imprimir página de teste: pede o IP e o formato (`pdf`, `pcl`, `text`, `ps`; vazio = automático). O transporte é escolhido sozinho: portas anunciadas primeiro, depois 9100, 631 e 515 |
| s | Servidor externo: URL WebSocket e token (`-` desativa ou remove) |
| h | Mostrar o menu |

Enter em branco cancela a maioria das perguntas. Exceções: na senha da rede WiFi significa rede aberta, e na URL ou token do servidor externo significa manter o valor atual. Senhas e token são ecoados como `*`.
