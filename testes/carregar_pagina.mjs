// Carrega o JavaScript da página do juiz no Node, sem navegador, para os
// testes chamarem as funções dela direto. O navegador é trocado por um
// "de mentira" que não faz nada: só o suficiente para o script carregar.
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const PAGINA = new URL('../firmware/cronometro/pagina/index.html', import.meta.url);

export function carregarPagina() {
  const html = readFileSync(PAGINA, 'utf-8');
  // o bloco principal é o último <script> (o primeiro, no <head>, só aplica o modo sol)
  const codigo = html.slice(html.lastIndexOf('<script>') + 8, html.lastIndexOf('</script>'));

  const nada = () => {};
  const elemento = () => ({ classList: { add: nada, remove: nada, toggle: nada, contains: () => false },
    setAttribute: nada, addEventListener: nada, append: nada, querySelector: () => null, querySelectorAll: () => [] });
  const ctx = {
    console, TextEncoder, TextDecoder, URL, performance,
    setTimeout, clearTimeout, setInterval: () => 0, clearInterval: nada,
    alert: nada, confirm: () => false,
    fetch: async () => { throw new Error('sem rede nos testes'); },
    location: { hash: '#/' },
    localStorage: { getItem: () => null, setItem: nada, removeItem: nada },
    document: { hidden: false, body: elemento(), addEventListener: nada, getElementById: () => null,
      querySelector: () => null, querySelectorAll: () => [], createElement: elemento },
    addEventListener: nada,
  };
  ctx.window = ctx;
  vm.createContext(ctx);
  vm.runInContext(codigo, ctx, { filename: 'index.html' });
  return ctx;
}
