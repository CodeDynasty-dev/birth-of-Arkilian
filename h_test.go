package main

import (
	"testing"
	"unsafe"
)

var globalBytes []byte

func BenchmarkUtf8ZeroCopy(b *testing.B) {
	str := "A🦊"
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		// This points the slice header to the string's existing memory
		globalBytes = unsafe.Slice(unsafe.StringData(str), len(str))
	}
}
