"""
Gera firmware/cronometro/web_page_gz.h a partir de
firmware/cronometro/pagina/index.html.

A página do juiz é editada como um .html comum e vai para o firmware
COMPACTADA (gzip): ~130 KB de texto viram ~38 KB de flash. O ESP32 manda
os bytes compactados com "Content-Encoding: gzip" e o navegador
descompacta sozinho.

Rode sempre que mudar pagina/index.html, ANTES de compilar/carregar:
    python ferramentas/gerar_pagina.py
(ou dê dois cliques em ferramentas/gerar_pagina.bat)

    --check   só confere se web_page_gz.h está em dia com o index.html
              (sai com erro se estiver desatualizado), sem gravar nada.

Só usa a biblioteca padrão do Python.
"""
import gzip
import hashlib
import re
import sys
from pathlib import Path

SKETCH = Path(__file__).resolve().parent.parent / "firmware" / "cronometro"
SOURCE = SKETCH / "pagina" / "index.html"
OUTPUT = SKETCH / "web_page_gz.h"


def build_header(html_bytes: bytes) -> str:
    sha = hashlib.sha256(html_bytes).hexdigest()[:16]
    # mtime=0: mesma entrada gera sempre os mesmos bytes (diff limpo no git)
    gz = gzip.compress(html_bytes, compresslevel=9, mtime=0)
    lines = []
    for i in range(0, len(gz), 20):
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in gz[i:i + 20]) + ",")
    return (
        "#pragma once\n"
        "#include <Arduino.h>\n"
        "\n"
        "// =====================================================================\n"
        "// WEB_PAGE_GZ.H — GERADO AUTOMATICAMENTE, NÃO EDITE À MÃO.\n"
        "//\n"
        "// Página do juiz (pagina/index.html) compactada com gzip por\n"
        "// ferramentas/gerar_pagina.py. Para mudar a página: edite\n"
        "// pagina/index.html e rode o script de novo antes de compilar.\n"
        "// Servida por handleRoot() em web_server.cpp com Content-Encoding: gzip.\n"
        "// =====================================================================\n"
        "\n"
        f"// index.html: {len(html_bytes)} bytes -> compactado: {len(gz)} bytes\n"
        f'#define INDEX_HTML_SOURCE_SHA "{sha}"\n'
        f"const size_t INDEX_HTML_GZ_LEN = {len(gz)};\n"
        "const uint8_t INDEX_HTML_GZ[] PROGMEM = {\n"
        + "\n".join(lines)
        + "\n};\n"
    )


def embedded_html(header_text: str) -> bytes:
    """Página guardada num web_page_gz.h, já descompactada (b"" se ilegível)."""
    try:
        start = header_text.index("PROGMEM = {") + len("PROGMEM = {")
        body = header_text[start:header_text.index("};", start)]
        data = bytes(int(tok, 16) for tok in re.findall(r"0x[0-9a-fA-F]{2}", body))
        return gzip.decompress(data)
    except (ValueError, OSError, EOFError):
        return b""


def main() -> int:
    # Quebra de linha do Windows (CRLF) vira a do Linux (LF) antes de tudo:
    # o Git pode guardar o arquivo de um jeito e entregar de outro, e o
    # resultado tem que ser o mesmo em qualquer computador (inclusive na
    # conferência automática do GitHub, que roda em Linux).
    html_bytes = SOURCE.read_bytes().replace(b"\r\n", b"\n")
    header = build_header(html_bytes)

    if "--check" in sys.argv:
        # Compara a página que está DENTRO do .h (descompactada) com o
        # index.html, e não os bytes compactados: zlibs de versões
        # diferentes (Windows x Linux) compactam o mesmo texto em bytes
        # diferentes, e os dois servem igual para o ESP32.
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if embedded_html(current) != html_bytes:
            print("web_page_gz.h DESATUALIZADO - rode: python ferramentas/gerar_pagina.py")
            return 1
        print("web_page_gz.h em dia com pagina/index.html")
        return 0

    OUTPUT.write_text(header, encoding="utf-8", newline="\n")
    gz_len = int(header.split("INDEX_HTML_GZ_LEN = ")[1].split(";")[0])
    print(f"OK: {SOURCE.name} ({len(html_bytes)} bytes) -> {OUTPUT.name} ({gz_len} bytes compactados)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
