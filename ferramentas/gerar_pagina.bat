@echo off
REM Gera web_page_gz.h a partir de pagina\index.html (ver gerar_pagina.py).
REM Rode sempre que mudar a pagina do juiz, antes de compilar no Arduino IDE.
python "%~dp0gerar_pagina.py"
pause
