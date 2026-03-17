/**
 * Encodes 20-dimensional coordinates into a single Morton code (Z-order).
 * @param coords An array of 20 numbers (integers).
 * @returns A BigInt representing the interleaved Morton code.
 */
function encodeMorton20D(coords: number[]): bigint {
  if (coords.length !== 20) {
    throw new Error("Exactly 20 coordinates are required.");
  }

  let mortonCode: bigint = 0n;

  // Determine the maximum bit length needed (e.g., if max coord is 15, we need 4 bits)
  const maxCoord = Math.max(...coords);
  const numBits = maxCoord > 0 ? maxCoord.toString(2).length : 1;

  for (let bitIdx = 0; bitIdx < numBits; bitIdx++) {
    for (let dimIdx = 0; dimIdx < 20; dimIdx++) {
      // 1. Extract the bit at 'bitIdx' from coordinate 'dimIdx'
      const bit = BigInt((coords[dimIdx] >> bitIdx) & 1);

      // 2. Shift it to its interleaved position: (bit index * total dimensions) + dimension index
      const position = BigInt(bitIdx * 20 + dimIdx);

      // 3. Place the bit into the final code
      mortonCode |= bit << position;
    }
  }

  return mortonCode;
}

// Example: Tiny coordinates to keep the output readable
// Let's set Coord 0 to 3 (binary 11) and Coord 1 to 1 (binary 01), others to 0
const input20D = new Array(20).fill(0);
input20D[0] = 3;
input20D[1] = 1;

const result = encodeMorton20D(input20D);
console.log(input20D);
console.log(`Morton Code (Decimal): ${result.toString()}`);
console.log(`Morton Code (Binary):  ${result.toString(2)}`);
