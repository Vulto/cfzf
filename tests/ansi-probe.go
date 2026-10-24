package fzf

import (
	"encoding/json"
	"fmt"
	"math/rand"
	"os"
	"testing"
	"unicode/utf8"
)

var zzBaseStrings = []string{
	"\x1b[0mhello world",
	"\x1b[1mhello world",
	"椙\x1b[1m椙",
	"椙\x1b[1椙m椙",
	"\x1b[1mhello \x1b[mw\x1b7o\x1b8r\x1b(Bl\x1b[2@d",
	"\x1b[1mhello \x1b[Kworld",
	"hello \x1b[34;45;1mworld",
	"hello \x1b[34;45;1mwor\x1b[34;45;1mld",
	"hello \x1b[34;45;1mwor\x1b[0mld",
	"hello \x1b[34;48;5;233;1mwo\x1b[38;5;161mr\x1b[0ml\x1b[38;5;161md",
	"hello \x1b[38;5;38;48;5;48;1mwor\x1b[38;5;48;48;5;38ml\x1b[0md",
	"hello \x1b[32;1mworld",
	"hello world",
	"hello \x1b[0;38;5;200;48;5;100mworld",
	"\x1b椙",
	"椙\x08",
	"\n\x08",
	"X\x08",
	"",
	"\x1b]4;3;rgb:aa/bb/cc\x07 ",
	"\x1b]4;3;rgb:aa/bb/cc\x1b\\ ",
}

func zzDump(strs []string, path string) {
	out := []map[string]any{}
	for _, s := range strs {
		seqs := []any{}
		rem := s
		for {
			a, b := nextAnsiEscapeSequence(rem)
			if a == -1 {
				break
			}
			seqs = append(seqs, []int{a, b})
			rem = rem[b:]
		}
		out = append(out, map[string]any{"s": s, "seqs": seqs})
	}
	b, _ := json.Marshal(out)
	os.WriteFile(path, b, 0644)
}

func TestZZProbe(t *testing.T) {
	zzDump(zzBaseStrings, "/tmp/opencode/ansi-probe.json")
	fmt.Println("wrote base")
}

func TestZZFuzz(t *testing.T) {
	repl := [...]rune{'\x0e', '\x0f', '\x1b', '\x08'}
	modify := func(s string, rr *rand.Rand) string {
		n := rr.Intn(len(s) + 1)
		b := []rune(s)
		for ; n >= 0 && len(b) != 0; n-- {
			i := rr.Intn(len(b))
			switch x := rr.Intn(4); x {
			case 0:
				b = append(b[:i], b[i+1:]...)
			case 1:
				b[i] = repl[rr.Intn(len(repl))]
			case 2:
				x := rune(rr.Intn(utf8.MaxRune))
				for !utf8.ValidRune(x) {
					x = rune(rr.Intn(utf8.MaxRune))
				}
				b[i] = x
			case 3:
				b[i] = rune(rr.Intn(utf8.MaxRune))
			}
		}
		return string(b)
	}
	rr := rand.New(rand.NewSource(1))
	var strs []string
	for _, s := range zzBaseStrings {
		for i := 0; i < 500; i++ {
			strs = append(strs, modify(s, rr))
		}
	}
	// random strings too
	for i := 0; i < 5000; i++ {
		n := rr.Intn(50)
		cps := make([]rune, n)
		for j := range cps {
			var r rune
			for range 1000 {
				r = rune(rr.Intn(utf8.MaxRune))
				if utf8.ValidRune(r) || rr.Float64() < 0.10 {
					break
				}
			}
			cps[j] = r
		}
		strs = append(strs, string(cps))
	}
	zzDump(strs, "/tmp/opencode/ansi-fuzz.json")
	fmt.Println("wrote fuzz", len(strs))
}
