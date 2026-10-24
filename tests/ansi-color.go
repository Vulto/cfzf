package fzf

import (
	"encoding/json"
	"fmt"
	"os"
	"testing"
)

func zzDumpColor(strs []string, path string) {
	type off struct {
		B, E       int32
		Fg, Bg, Ul int32
		Attr       int32
		Lbg        int32
	}
	out := []map[string]any{}
	for _, s := range strs {
		trimmed, offsets, st := extractColor(s, nil, nil)
		os_ := []off{}
		if offsets != nil {
			for _, o := range *offsets {
				os_ = append(os_, off{o.offset[0], o.offset[1],
					int32(o.color.fg), int32(o.color.bg), int32(o.color.ul),
					int32(o.color.attr), int32(o.color.lbg)})
			}
		}
		var fg, bg, ul, at, lbg int32
		if st != nil {
			fg, bg, ul, at, lbg = int32(st.fg), int32(st.bg), int32(st.ul), int32(st.attr), int32(st.lbg)
		} else {
			fg, bg, ul, at, lbg = -2, -2, -2, -2, -2
		}
		out = append(out, map[string]any{
			"in": s, "out": trimmed, "offsets": os_,
			"st": []int32{fg, bg, ul, at, lbg},
		})
	}
	b, _ := json.Marshal(out)
	os.WriteFile(path, b, 0644)
}

func TestZZColor(t *testing.T) {
	strs := []string{
		"hello world",
		"\x1b[0mhello world",
		"\x1b[31mred",
		"\x1b[1;31mboldred\x1b[0m",
		"\x1b[38;5;161mpink\x1b[0m plain",
		"\x1b[48;5;100mbgonly",
		"a\x1b[32mb\x1b[33mc",
		"\x1b[38;2;10;20;30mrgb",
		"\x1b[1mbold\x1b[22mnormal",
		"\x1b[4munder\x1b[24mplain",
		"\x1b[7mreverse\x1b[27m",
		"line1\x1b[41m\nline2",
		"\x1b[38;5;200;48;5;100mmulti\x1b[39;49m",
		"\x1b[9;3;4:2mstyled\x1b[0m",
		"",
	}
	zzDumpColor(strs, "/tmp/opencode/ansi-color.json")
	fmt.Println("wrote color", len(strs))
}
