#!/usr/bin/env python3
r"""
split_zip_parts.py — agrupa o conteúdo de uma pasta em vários .zip
INDEPENDENTES de ~N MB cada (cada zip abre sozinho, sem depender dos outros).

Uso:
    python split_zip_parts.py <pasta_origem> <pasta_destino> [--size-mb 256] [--prefix game_part]

Exemplo:
    python split_zip_parts.py "C:\Jogo" "C:\saida" --size-mb 256 --prefix game_part

Gera: game_part1.zip, game_part2.zip, game_part3.zip ...
Cada um já pronto para subir no CDN e ser baixado/extraído independentemente
pelo installer (sem concatenação, sem volume multivolume).

Lógica:
- Caminha por todos os arquivos da pasta de origem (recursivo).
- Agrupa em "baldes" (bins) usando bin-packing guloso (greedy): vai
  enchendo o zip atual até estourar o limite, daí abre um novo.
- Nenhum arquivo é cortado — se um arquivo sozinho for maior que o
  limite, ele vira um zip próprio (maior que N MB, mas íntegro).
- Caminhos relativos dentro de cada zip são preservados (mesma estrutura
  de pastas que existia na origem), então a extração reconstrói o jogo
  corretamente independente de quantas partes existirem.
"""

import argparse
import os
import sys
import zipfile


def collect_files(src_dir):
    """Retorna lista de (caminho_absoluto, caminho_relativo, tamanho_em_bytes)."""
    items = []
    for root, _dirs, files in os.walk(src_dir):
        for name in files:
            abs_path = os.path.join(root, name)
            rel_path = os.path.relpath(abs_path, src_dir)
            try:
                size = os.path.getsize(abs_path)
            except OSError as e:
                print(f"[aviso] pulando '{abs_path}': {e}", file=sys.stderr)
                continue
            items.append((abs_path, rel_path, size))
    return items


def pack_bins(items, limit_bytes):
    """Bin-packing guloso: cada bin é uma lista de itens cuja soma <= limit
    (exceto quando um único arquivo já excede o limite — vai sozinho)."""
    # arquivos maiores primeiro ajuda a aproveitar melhor o espaço dos baldes
    items_sorted = sorted(items, key=lambda x: x[2], reverse=True)

    bins = []          # lista de bins, cada bin é lista de itens
    bin_sizes = []      # tamanho atual de cada bin

    for item in items_sorted:
        _abs_path, _rel_path, size = item
        placed = False

        # tenta encaixar no bin existente com mais espaço sobrando que comporte
        best_idx = -1
        best_remaining = -1
        for i, cur_size in enumerate(bin_sizes):
            remaining = limit_bytes - cur_size
            if size <= remaining and remaining > best_remaining:
                best_idx = i
                best_remaining = remaining

        if best_idx >= 0:
            bins[best_idx].append(item)
            bin_sizes[best_idx] += size
            placed = True

        if not placed:
            # novo bin (também cobre o caso do arquivo ser maior que o limite)
            bins.append([item])
            bin_sizes.append(size)

    return bins


def write_zips(bins, dest_dir, prefix):
    os.makedirs(dest_dir, exist_ok=True)
    total = len(bins)

    for idx, bin_items in enumerate(bins, start=1):
        zip_name = f"{prefix}{idx}.zip"
        zip_path = os.path.join(dest_dir, zip_name)
        bin_total_size = sum(size for _a, _r, size in bin_items)

        print(f"[{idx}/{total}] criando {zip_name} "
              f"({bin_total_size / (1024*1024):.1f} MB, {len(bin_items)} arquivo(s))...")

        with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED,
                              compresslevel=6) as zf:
            for abs_path, rel_path, _size in bin_items:
                # normaliza separadores para sempre usar "/" dentro do zip
                arcname = rel_path.replace(os.sep, "/")
                zf.write(abs_path, arcname=arcname)

    print(f"\nPronto: {total} arquivo(s) zip gerado(s) em '{dest_dir}'.")


def main():
    parser = argparse.ArgumentParser(
        description="Agrupa arquivos de uma pasta em zips independentes de ~N MB cada."
    )
    parser.add_argument("src_dir", help="Pasta de origem (com os arquivos do jogo)")
    parser.add_argument("dest_dir", help="Pasta de destino (onde os .zip serão criados)")
    parser.add_argument("--size-mb", type=float, default=256,
                         help="Tamanho alvo de cada zip em MB (padrão: 256)")
    parser.add_argument("--prefix", default="game_part",
                         help="Prefixo do nome dos zips (padrão: game_part -> game_part1.zip, game_part2.zip...)")

    args = parser.parse_args()

    if not os.path.isdir(args.src_dir):
        print(f"Erro: pasta de origem '{args.src_dir}' não existe.", file=sys.stderr)
        sys.exit(1)

    limit_bytes = int(args.size_mb * 1024 * 1024)

    print(f"Coletando arquivos de '{args.src_dir}'...")
    items = collect_files(args.src_dir)
    if not items:
        print("Nenhum arquivo encontrado.", file=sys.stderr)
        sys.exit(1)

    total_size = sum(size for _a, _r, size in items)
    print(f"{len(items)} arquivo(s), {total_size / (1024*1024):.1f} MB no total.")

    bins = pack_bins(items, limit_bytes)
    write_zips(bins, args.dest_dir, args.prefix)


if __name__ == "__main__":
    main()