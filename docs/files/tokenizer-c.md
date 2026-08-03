# `src/tokenizer.c` — byte-level BPE

**~740 lines.** GPT-2 style byte-level BPE, matching llama.cpp's
`llm_tokenizer_bpe` for the `qwen35` pre-tokeniser.

## Three stages

1. **Special-token scan.** `<|im_start|>` and friends are matched
   literally, longest-first, and never split.
2. **Pre-tokenisation.** The input is cut into words by the qwen35
   regex.
3. **Byte-level BPE.** Each word maps through the GPT-2 byte alphabet,
   then merges greedily by rank from the GGUF merge table.

## No regex library

The qwen35 pattern is:

```
(?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])
|[^\r\n\p{L}\p{N}]?[\p{L}\p{M}]+
|\p{N}
| ?[^\s\p{L}\p{M}\p{N}]+[\r\n]*
|\s*[\r\n]+
|\s+(?!\S)
|\s+
```

`split_words()` implements this directly as a hand-written scanner —
seven alternatives, tried in order. Adding a regex engine would have
been more code than the scanner, and slower.

The `\p{L}` / `\p{M}` / `\p{N}` classes come from `unicode_tbl.h`, a
generated table of codepoint ranges (660 + 182 + 143 ranges), searched
by binary search. `tools/gen_unicode.py` regenerates it; the build never
needs Python.

Note `\p{N}` matches **one digit at a time** — `1234567890` becomes ten
tokens. That is correct for this model family and a common source of
confusion.

## GPT-2 byte alphabet

Every byte maps to a printable codepoint (hence `Ġ` for space). The
table is built once at startup, along with the reverse map used by
`tok_piece`.

## BPE merges

Symbols live in a doubly linked list so a merge is O(1). Each pass scans
for the lowest-rank adjacent pair and merges it. O(n²) per word in
theory, but words are short — this never shows up in a profile.

## Testing

`tests/t_tokenizer.c` round-trips 14 strings: contractions, CJK, emoji,
accents, tabs/newlines, code punctuation, whitespace runs, and control
tokens. Every one must decode back to the exact input.

## See also

- [../FINDINGS.md](../FINDINGS.md) for the UTF-8 streaming issue
