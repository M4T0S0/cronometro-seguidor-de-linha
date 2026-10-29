// Testes da página do juiz. Rodar na raiz do repositório:
//   node --test
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { carregarPagina } from './carregar_pagina.mjs';

const p = carregarPagina();
// Objetos criados dentro da página vêm de outro "mundo" do JavaScript;
// isto os traz para cá para o deepEqual comparar só os valores.
const limpo = (v) => JSON.parse(JSON.stringify(v));

test('formatElapsed: tempo em mm:ss.mmm', () => {
  assert.equal(p.formatElapsed(0), '00:00.000');
  assert.equal(p.formatElapsed(12_345_000), '00:12.345');
  assert.equal(p.formatElapsed(61_001_000), '01:01.001');
  assert.equal(p.formatElapsed(12_345_999), '00:12.345'); // corta, não arredonda
  assert.equal(p.formatElapsed(null), '--:--.---');
});

test('escapeHtml e initials não deixam passar HTML', () => {
  assert.equal(p.escapeHtml('<b>"Ó" & \'x\'</b>'), '&lt;b&gt;&quot;Ó&quot; &amp; &#39;x&#39;&lt;/b&gt;');
  assert.equal(p.initials('Seguidor de Linha'), 'SL');
  assert.equal(p.initials('coruja'), 'C');
  assert.equal(p.initials(''), '?');
  assert.ok(!p.initials('<x y>').includes('<'));
});

test('busca: só pelo começo do nome, sem acento nem maiúscula', () => {
  const casa = (nome, busca) => p.wordPrefixIndex(nome, busca) === 0;
  assert.ok(casa('Ventania', 've'));
  assert.ok(!casa('Overlord', 've'));            // "ve" no meio não conta
  assert.ok(!casa('Seguidor de Linha', 'd'));    // nem no começo de outra palavra
  assert.ok(casa('Dragão', 'dragao'));
  assert.ok(casa('dragao', 'DRAGÃO'));
  assert.ok(!casa('Raio', ''));
  assert.equal(p.highlightMatch('Dragão', 'dra'), '<mark class="hit">Dra</mark>gão');
  assert.equal(p.highlightMatch('<Raio>', 'x'), '&lt;Raio&gt;');
});

test('searchRoster: acha robôs e equipes pelo começo do nome', () => {
  const grupos = [
    { team: { id: 1, name: 'Overlord' }, robots: [{ id: 1, name: 'Seguidor de Linha' }, { id: 2, name: 'Dinamo' }] },
    { team: { id: 2, name: 'Dragões' }, robots: [{ id: 3, name: 'Ventania' }] },
  ];
  const r = p.searchRoster(grupos, 'd');
  assert.deepEqual(limpo(r.robots.map(x => x.p.name)), ['Dinamo']);
  assert.deepEqual(limpo(r.teams.map(g => g.team.name)), ['Dragões']);
});

test('planilha CSV do Excel (ponto e vírgula, cabeçalho, BOM, CRLF)', () => {
  const csv = '﻿equipe;robo;codigo\r\nEquipe Falcão;Falcão 1;F01\r\n\r\nCoruja;Coruja;\r\n';
  assert.deepEqual(limpo(p.parseCsvText(csv)), [
    { team: 'Equipe Falcão', name: 'Falcão 1', code: 'F01' },
    { team: 'Coruja', name: 'Coruja', code: '' },
  ]);
});

test('planilha CSV: colunas em outra ordem, nomes alternativos e aspas', () => {
  const csv = 'Nome do robô,Código,Time\n"Raio, o veloz",R1,"Dinamo ""UTF"""\n';
  assert.deepEqual(limpo(p.parseCsvText(csv)), [{ team: 'Dinamo "UTF"', name: 'Raio, o veloz', code: 'R1' }]);
});

test('planilha CSV sem cabeçalho usa equipe, robô, código', () => {
  assert.deepEqual(limpo(p.parseCsvText('Overlord;Hiper;H9\n')), [{ team: 'Overlord', name: 'Hiper', code: 'H9' }]);
  assert.deepEqual(limpo(p.parseCsvText('\n\n')), []);
});

test('importação JSON: lista simples, agrupada por equipe e dentro de objeto', () => {
  assert.deepEqual(limpo(p.parseJsonText('[{"equipe":"A","robo":"R1","codigo":7}]')),
    [{ team: 'A', name: 'R1', code: '7' }]);
  assert.deepEqual(limpo(p.parseJsonText('{"equipes":[{"nome":"B","robos":["X",{"nome":"Y","codigo":"C2"}]}]}')),
    [{ team: 'B', name: 'X', code: '' }, { team: 'B', name: 'Y', code: 'C2' }]);
  assert.throws(() => p.parseJsonText('{"a":1}'), /nenhuma lista/);
});

test('validação da importação aponta cada problema', () => {
  const linhas = p.validateImportRows([
    { team: ' Equipe ', name: ' Robô ', code: '' },
    { team: '', name: 'Sem equipe', code: '' },
    { team: 'X', name: '', code: '' },
    { team: 'X', name: 'Com "aspas"', code: '' },
    { team: 'X', name: 'ã'.repeat(20), code: '' },   // 20 letras, mas 40 bytes
    { team: 'equipe', name: 'robo', code: '' },        // repetido (sem acento/maiúscula)
  ]);
  assert.deepEqual(linhas.map(l => l.error), ['', 'sem equipe', 'sem nome do robô',
    'aspas (") não são permitidas', 'nome do robô longo demais', 'repetido no arquivo']);
  assert.equal(linhas[0].team, 'Equipe');
});

test('validateEditName', () => {
  assert.equal(p.validateEditName('Falcão', 'do robô'), '');
  assert.match(p.validateEditName('', 'do robô'), /Informe/);
  assert.match(p.validateEditName('"X"', 'do robô'), /Aspas/);
});

test('fase da tela da competição segue a prioridade certa', () => {
  const comp = { status: 'draft' };
  const dia = { day_complete: false };
  const desarmado = { armed: false };
  const fase = (c, d, a, runs = []) => p.derivePhase(c, d, a, runs).phase;

  assert.equal(fase(comp, dia, desarmado), 'select');
  assert.equal(fase({ status: 'finished' }, dia, desarmado), 'finished');
  assert.equal(fase(comp, dia, { armed: true, released: false }), 'locked');
  assert.equal(fase(comp, dia, { armed: true, released: true, started: false }), 'waiting_start');
  assert.equal(fase(comp, dia, { armed: true, released: true, started: true }), 'running');
  // validação pendente vem antes de tudo (menos competição encerrada)
  assert.equal(fase(comp, dia, { armed: true, released: false }, [{ status: 'pending_validation' }]), 'validate');
  // robô armado (ex.: tomada repetida) vem antes de "dia concluído"
  assert.equal(fase(comp, { day_complete: true }, { armed: true, released: false }), 'locked');
  assert.equal(fase(comp, { day_complete: true }, desarmado), 'day_complete');
});

const RANKING = [
  { position: 1, participant_id: 1, participant_name: 'Coruja', team_name: 'Coruja Robotics', best_elapsed_us: 11_876_000,
    attempts: [{ day: 1, attempt: 1, elapsed_us: 11_876_000, status: 'valid', is_best: true }, null] },
  { position: 2, participant_id: 2, participant_name: '<Falcão>', team_name: '', best_elapsed_us: -1,
    attempts: [{ day: 1, attempt: 1, elapsed_us: 14_020_000, status: 'invalid', is_best: false }, null] },
];

test('ranking do celular: melhor tempo, "sem tempo" e nome escapado', () => {
  const html = p.renderRankingList(RANKING);
  assert.match(html, /rk-best">00:11.876</);
  assert.match(html, /rk-best none">sem tempo</);
  assert.ok(html.includes('&lt;Falcão&gt;') && !html.includes('<Falcão>'));
  assert.match(html, /att-invalid/);
  assert.match(p.renderRankingList([]), /Nenhum resultado/);
});

test('ranking do Painel: só posição, robô e melhor tempo (legível de longe)', () => {
  const html = p.renderTvTable(RANKING);
  assert.match(html, /<th>Melhor tempo<\/th>/);
  assert.match(html, /tv-best">00:11.876</);
  assert.match(html, /tv-best none">sem tempo</);
  assert.ok(html.includes('&lt;Falcão&gt;') && !html.includes('<Falcão>'));
  assert.ok(!html.includes('00:14.020'));                     // tomadas uma a uma ficam fora da TV
  assert.match(p.renderTvTable([]), /Nenhum resultado/);
});

test('Painel mostra quem está na pista e o tempo correndo', () => {
  const pronto = p.renderTvOnTrack({ participant_name: '<Raio>', day: 1, attempt: 2, released: false, started: false });
  assert.match(pronto, /Na pista/);
  assert.ok(pronto.includes('&lt;Raio&gt;') && !pronto.includes('live-timer'));
  const correndo = p.renderTvOnTrack({ participant_name: 'Raio', day: 1, attempt: 2, released: true, started: true, elapsed_us: 5_000_000 });
  assert.match(correndo, /Correndo/);
  assert.match(correndo, /id="live-timer">00:05.000</);
});

test('attemptClass e plural', () => {
  assert.equal(p.attemptClass({ status: 'valid' }), 'att-valid');
  assert.equal(p.attemptClass({ status: 'dnf' }), 'att-dnf');
  assert.equal(p.attemptClass({ status: 'pending_validation' }), 'att-pending');
  assert.equal(p.attemptClass({ status: 'invalid' }), 'att-invalid');
  assert.equal(p.plural(1, 'robô', 'robôs'), '1 robô');
  assert.equal(p.plural(0, 'robô', 'robôs'), '0 robôs');
});

test('planilha do ranking abre certo no Excel', () => {
  const csv = p.rankingCsv([
    { position: 1, participant_name: 'Coruja', team_name: 'A; B', best_elapsed_us: 11_876_000,
      attempts: [{ day: 1, attempt: 1, elapsed_us: 11_876_000, status: 'valid' }, { day: 1, attempt: 2, elapsed_us: 0, status: 'dnf' }] },
    { position: 2, participant_name: 'Raio "R"', team_name: '', best_elapsed_us: -1,
      attempts: [{ day: 1, attempt: 1, elapsed_us: 14_020_000, status: 'invalid' }, null] },
  ]);
  const linhas = csv.replace(/^﻿/, '').trim().split('\r\n');
  assert.ok(csv.startsWith('﻿'));                        // BOM: Excel reconhece os acentos
  assert.equal(linhas[0], 'Posição;Robô;Equipe;Melhor tempo;Dia 1 tomada 1;Dia 1 tomada 2');
  assert.equal(linhas[1], '1º;Coruja;"A; B";00:11.876;00:11.876;Não completou');
  assert.equal(linhas[2], '2º;"Raio ""R""";;sem tempo;00:14.020 (inválida);');
});

test('vibra só quando aparece um resultado novo para validar', () => {
  const vibrou = [];
  p.navigator = { vibrate: (padrao) => vibrou.push(padrao) };
  const pendente = (id) => ({ phase: 'validate', pendingRun: { event_id: id } });
  p.vibrateOnNewResult(pendente('a'));          // tela acabou de abrir: não vibra
  p.vibrateOnNewResult(pendente('a'));          // mesma corrida: não vibra de novo
  p.vibrateOnNewResult({ phase: 'select' });
  p.vibrateOnNewResult(pendente('b'));          // resultado novo: vibra
  assert.equal(vibrou.length, 1);
  delete p.navigator;
});
