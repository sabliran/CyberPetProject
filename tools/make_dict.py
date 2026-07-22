#!/usr/bin/env python3
"""make_dict.py — PC-side dictionary generator (dictionary app, phase 2).

Turns WordNet 3.1 into the two flat files the firmware reads from SD:

  tools/dict_out/words.idx   fixed-width records, sorted ascending by key:
                               [key: DICT_KEY_LEN bytes, a-z only, NUL-padded]
                               [offset: uint32 LE into defs.dat]
                               [length: uint32 LE]
  tools/dict_out/defs.dat    per-word UTF-8 text record:
                               line 1: display form(s), "/"-separated if merged
                               then one sense per line: "pos|definition"
                               (pos = n/v/adj/adv), senses in WordNet order.
  tools/dict_out/dict_format.h  generated constants for the firmware
                               (DICT_KEY_LEN, DICT_RECORD_SIZE, DICT_WORD_COUNT).
                               Lives here for now; a later phase moves it.

Data source: the NLTK "wordnet31" corpus (WordNet 3.1 database files, read
straight from the downloaded zip). NOTE: nltk's WordNetCorpusReader also
requires the base "wordnet" (3.0) corpus to be present — its __init__
builds a 3.0->3.1 sense map from it. That corpus is an nltk-internal
dependency only; every lemma/definition emitted here comes from 3.1.

Run with the venv python (stdlib + nltk only):
  tools/.venv-dict/bin/python tools/make_dict.py

Idempotent: corpora download only if missing, outputs are rewritten whole.
Exits nonzero if any self-test fails.
"""

import os
import random
import struct
import sys
import unicodedata

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dict_out")
IDX_PATH = os.path.join(OUT_DIR, "words.idx")
DEFS_PATH = os.path.join(OUT_DIR, "defs.dat")
HDR_PATH = os.path.join(OUT_DIR, "dict_format.h")

# WordNet POS tags -> the format's pos strings. 's' (satellite adjective)
# folds into 'adj' — the distinction is WordNet-internal.
POS_MAP = {"n": "n", "v": "v", "a": "adj", "s": "adj", "r": "adv"}


def make_key(lemma):
    """Search key: lowercase a-z only. Diacritics transliterated (NFKD strip),
    everything else (spaces, hyphens, apostrophes, digits, periods,
    underscores) dropped."""
    decomposed = unicodedata.normalize("NFKD", lemma.lower())
    return "".join(c for c in decomposed if "a" <= c <= "z")


def display_form(lemma):
    """Original headword; WordNet's underscores and hyphens both become
    spaces ("ice_cream" -> "ice cream", "self-esteem" -> "self esteem")."""
    return lemma.replace("_", " ").replace("-", " ")


def load_wordnet():
    import nltk
    from nltk.corpus.reader.wordnet import WordNetCorpusReader

    for corpus in ("wordnet31", "wordnet"):  # "wordnet" = reader dep, see docstring
        try:
            nltk.data.find(f"corpora/{corpus}.zip")
        except LookupError:
            print(f"downloading nltk corpus {corpus}...")
            if not nltk.download(corpus, quiet=True):
                sys.exit(f"FATAL: nltk.download('{corpus}') failed")
    wn = WordNetCorpusReader(nltk.data.find("corpora/wordnet31.zip/wordnet31/"), None)
    version = wn.get_version()
    if version != "3.1":
        sys.exit(f"FATAL: expected WordNet 3.1, reader reports {version!r}")
    return wn


def build_entries(wn):
    """Returns ({key: {"forms": [...], "senses": [(pos, def), ...]}},
    dropped_short, merged_collisions)."""
    entries = {}
    dropped = 0
    merged = 0
    for lemma in wn.all_lemma_names():  # lowercase, underscored index lemmas
        key = make_key(lemma)
        if len(key) < 2:
            dropped += 1
            continue
        forms = []
        senses = []
        for synset in wn.synsets(lemma):  # WordNet sense order per POS
            # Recover the original-case headword ("Earth" for index lemma
            # "earth") from the synset's own lemma list.
            name = lemma
            for cand in synset.lemmas():
                if cand.name().lower() == lemma:
                    name = cand.name()
                    break
            form = display_form(name)
            if form not in forms:
                forms.append(form)
            # Definitions are single-line in WordNet; strip defensively so a
            # stray newline can never break the one-sense-per-line format.
            definition = " ".join(synset.definition().split())
            senses.append((POS_MAP[synset.pos()], definition))
        if not senses:
            continue
        if key in entries:  # collision ("re-cover"/"recover"): merge
            merged += 1
            entry = entries[key]
            for form in forms:
                if form not in entry["forms"]:
                    entry["forms"].append(form)
            existing = set(entry["senses"])
            entry["senses"].extend(s for s in senses if s not in existing)
        else:
            entries[key] = {"forms": forms, "senses": senses}
    return entries, dropped, merged


def emit(entries):
    """Writes defs.dat + words.idx + dict_format.h. Returns key_len."""
    longest = max(len(k) for k in entries)
    key_len = (longest + 3) // 4 * 4  # round up to a multiple of 4
    record_size = key_len + 8
    print(f"longest key: {longest} letters -> DICT_KEY_LEN {key_len} "
          f"(record {record_size} B)")

    os.makedirs(OUT_DIR, exist_ok=True)
    keys = sorted(entries)
    max_record = 0
    with open(DEFS_PATH, "wb") as defs, open(IDX_PATH, "wb") as idx:
        for key in keys:
            entry = entries[key]
            lines = ["/".join(entry["forms"])]
            lines += [f"{pos}|{definition}" for pos, definition in entry["senses"]]
            blob = ("\n".join(lines) + "\n").encode("utf-8")
            max_record = max(max_record, len(blob))
            offset = defs.tell()
            defs.write(blob)
            idx.write(key.encode("ascii").ljust(key_len, b"\0"))
            idx.write(struct.pack("<II", offset, len(blob)))

    with open(HDR_PATH, "w") as hdr:
        hdr.write(
            "// Generated by tools/make_dict.py from WordNet 3.1 - do NOT\n"
            "// hand-edit; regenerate there and re-copy the CyberPet/ copy.\n"
            "// words.idx record: [key: DICT_KEY_LEN bytes, a-z, NUL-padded]\n"
            "//                   [offset: uint32 LE] [length: uint32 LE]\n"
            "// DICT_MAX_RECORD_LEN sizes the reader's defs.dat text buffer\n"
            "// (largest merged entry, e.g. all atomic_number_* collide).\n"
            "#pragma once\n"
            f"#define DICT_KEY_LEN    {key_len}\n"
            f"#define DICT_RECORD_SIZE {record_size}\n"
            f"#define DICT_WORD_COUNT {len(keys)}\n"
            f"#define DICT_MAX_RECORD_LEN {max_record}\n"
        )
    print(f"largest defs.dat record: {max_record} B -> DICT_MAX_RECORD_LEN")
    return key_len


# ── Self-tests: reopen the emitted files cold and act like the firmware ─────

def idx_lookup(idx, key_len, count, key):
    """Binary search words.idx exactly the way the firmware will: fixed-width
    records, bytewise compare on the NUL-padded key."""
    record_size = key_len + 8
    want = key.encode("ascii").ljust(key_len, b"\0")
    lo, hi = 0, count - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        idx.seek(mid * record_size)
        record = idx.read(record_size)
        got = record[:key_len]
        if got == want:
            return struct.unpack("<II", record[key_len:])
        if got < want:
            lo = mid + 1
        else:
            hi = mid - 1
    return None


def self_test(entries, key_len):
    failures = []
    record_size = key_len + 8
    keys = sorted(entries)

    with open(IDX_PATH, "rb") as idx:
        idx_size = os.fstat(idx.fileno()).st_size
        if idx_size % record_size:
            failures.append(f"words.idx size {idx_size} not a multiple of {record_size}")
        count = idx_size // record_size

        # Sorted + every key fits, over the whole file.
        prev = b""
        for i in range(count):
            idx.seek(i * record_size)
            raw = idx.read(key_len)
            if raw <= prev:
                failures.append(f"record {i}: not strictly ascending")
                break
            if raw.rstrip(b"\0").ljust(key_len, b"\0") != raw:
                failures.append(f"record {i}: key not NUL-padded a-z")
                break
            prev = raw
        print(f"sorted+width check: {count} records "
              f"{'OK' if not failures else 'FAIL'}")

        # 20 random known keys: binary-search cold, verify the offset/length
        # land on the right defs.dat record.
        rng = random.Random(0xD1C7)  # fixed seed: idempotent runs
        with open(DEFS_PATH, "rb") as defs:
            defs_size = os.fstat(defs.fileno()).st_size
            for key in rng.sample(keys, 20):
                hit = idx_lookup(idx, key_len, count, key)
                if hit is None:
                    failures.append(f"known key {key!r}: not found")
                    continue
                offset, length = hit
                if offset + length > defs_size:
                    failures.append(f"{key!r}: offset+length past defs.dat end")
                    continue
                defs.seek(offset)
                blob = defs.read(length)
                # Record boundaries: preceding byte (if any) and last byte
                # must both be '\n'.
                if offset:
                    defs.seek(offset - 1)
                    if defs.read(1) != b"\n":
                        failures.append(f"{key!r}: offset lands mid-record")
                if not blob.endswith(b"\n"):
                    failures.append(f"{key!r}: record not newline-terminated")
                first = blob.decode("utf-8").split("\n", 1)[0]
                if not any(make_key(f) == key for f in first.split("/")):
                    failures.append(f"{key!r}: display line {first!r} mismatch")
        misses = ["qq", "zzzzq", "aaqz", "xxjq", "qzqzqz"]
        for miss in misses:
            assert miss not in entries, f"test bug: {miss!r} exists"
            if idx_lookup(idx, key_len, count, miss) is not None:
                failures.append(f"miss {miss!r}: unexpectedly found")
        print(f"binary-search spot check: 20 hits + {len(misses)} misses "
              f"{'OK' if not failures else 'FAIL'}")
    return count, failures


def main():
    wn = load_wordnet()
    print("building entries from WordNet", wn.get_version(), "...")
    entries, dropped, merged = build_entries(wn)
    key_len = emit(entries)
    count, failures = self_test(entries, key_len)

    print(f"\ntotal entries:          {count}")
    print(f"dropped (<2 letters):   {dropped}")
    print(f"merged collisions:      {merged}")
    print(f"words.idx:              {os.path.getsize(IDX_PATH):,} bytes")
    print(f"defs.dat:               {os.path.getsize(DEFS_PATH):,} bytes")

    # Largest two-letter-prefix buckets: sizes the firmware's search cap.
    buckets = {}
    for key in entries:
        buckets[key[:2]] = buckets.get(key[:2], 0) + 1
    print("\n15 largest two-letter-prefix buckets:")
    for prefix, n in sorted(buckets.items(), key=lambda kv: -kv[1])[:15]:
        print(f"  {prefix}: {n}")

    if failures:
        print("\nSELF-TEST FAILURES:")
        for failure in failures:
            print("  " + failure)
        sys.exit(1)

    print("\nall self-tests passed. Copy to the SD card with:")
    card = "<CARD>"
    media = os.path.join("/run/media", os.environ.get("USER", "user"))
    if os.path.isdir(media):
        mounts = [m for m in sorted(os.listdir(media))
                  if os.path.isdir(os.path.join(media, m))]
        if len(mounts) == 1:
            card = mounts[0]
    print(f'  mkdir -p "/run/media/$USER/{card}/dict" && '
          f'cp "{IDX_PATH}" "{DEFS_PATH}" "/run/media/$USER/{card}/dict/"')


if __name__ == "__main__":
    main()
