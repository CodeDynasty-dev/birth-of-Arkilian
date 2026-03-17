package main

import "fmt"

func main() {
    // Test 1: Prefix similarity
    strings := []string{
        "ari",
        "bri",
        "cri",
        "dri",
        "eri",
        "fri",  
        "mri",
        "zri",
        "LlEeGgOo bricks",
    }
    
    for _, s := range strings {
        bytes := []byte(s)
        // Compute first 4 bytes as uint32 coordinate
        var coord uint32
        for i := 0; i < len(bytes) && i < 4; i++ {
            coord |= uint32(bytes[i]) << (24 - i*8)
        }
        fmt.Printf("%-20s → %08X → %v\n", s, coord, bytes)
    }
    
    // Test 2: Measure "distance" between strings
    fmt.Println("\nByte distances:")
    s1 := []byte("friday candour")
    s2 := []byte("friday candou")
    
    // Compare byte-by-byte
    diff := 0
    for i := 0; i < len(s1) && i < len(s2); i++ {
        if s1[i] != s2[i] {
            diff++
        }
    }
    diff += abs(len(s1) - len(s2))
    fmt.Printf("Distance: %d bytes\n", diff)
}

func abs(x int) int {
    if x < 0 { return -x }
    return x
}

 