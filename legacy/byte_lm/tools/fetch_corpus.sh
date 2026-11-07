#!/usr/bin/env bash
# Descarga textos de dominio público en español a orion_home/data para entrenar con datos de verdad.
# Orion mejora muchísimo con más texto: con ~15 KB solo memoriza; con varios MB empieza a generalizar.
# (Necesita acceso a internet; añade tus propios .txt a orion_home/data cuando quieras.)
set -euo pipefail
HOME_DIR=${1:-orion_home}
mkdir -p "$HOME_DIR/data"
get() {  # id_gutenberg nombre
  echo "descargando $2 ..."
  curl -fsSL "https://www.gutenberg.org/cache/epub/$1/pg$1.txt" -o "$HOME_DIR/data/$2.txt"
}
get 2000 quijote
echo "listo: $(du -ch "$HOME_DIR"/data/*.txt | tail -n1)"
