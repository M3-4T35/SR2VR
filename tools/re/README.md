# Outils d'analyse statique de SR2_pc.exe

Petits scripts Python (capstone) pour lire l'exe du jeu sans le lancer. Ils travaillent sur une
copie de `SR2_pc.exe` (celui de Steam modifié par Juiced, base 0x00400000) dont le chemin est
en tête de `sr2.py` (`PATH`), et se lancent depuis ce dossier.

    pip install capstone
    python3 sr2.py uf 9C0500 300      # désassemble une fonction
    python3 sr2.py dis 4F9B14 12      # n instructions
    python3 sr2.py ctx 9C126B 20 6    # instructions autour d'une adresse
    python3 sr2.py calls 9D9090       # appels directs vers une fonction
    python3 sr2.py refs 2527262       # instructions qui citent une adresse absolue
    python3 sr2.py func 9C1264        # début probable de la fonction

`who.py` (à quelle fonction appartient une adresse, et ses appelants), `offs.py` et `offs2.py`
(instructions qui lisent un champ `[reg + décalage]`, avec la fonction qui les contient) ont
besoin de `calltargets.pkl`, la table des cibles d'appel, à construire une fois :

    python3 - <<'PY'
    import sys, collections, pickle, struct; sys.argv = ['x', 'x']
    exec(open('sr2.py').read().split("if __name__")[0])
    a0, tb = text_bytes(); tg = collections.defaultdict(list); i = tb.find(b'\xe8')
    while i != -1:
        if i + 5 <= len(tb):
            t = (a0 + i + 5 + struct.unpack_from('<i', tb, i + 1)[0]) & 0xffffffff
            if a0 <= t < a0 + len(tb): tg[t].append(a0 + i)
        i = tb.find(b'\xe8', i + 1)
    pickle.dump(dict(tg), open('calltargets.pkl', 'wb'))
    PY

`sig.py "8B C4 ?? 89" ...` compte les occurrences d'une signature dans le code : une signature
du mod doit en avoir exactement une.
