/**
 * fib190 - 斐波那契数列前 190 项计算模块
 *
 * 用法:
 *   import { fibonacci, fibonacciSequence, fibonacciAt } from "./fib190";
 *
 *   // 生成前 190 项（数组）
 *   const seq = fibonacciSequence();
 *
 *   // 获取第 n 项（0-indexed，F(0)=0, F(1)=1）
 *   const f100 = fibonacciAt(100);
 *
 *   // 按需生成（迭代器）
 *   for (const [i, val] of fibonacci()) {
 *       console.log(`F(${i}) = ${val}`);
 *       if (i >= 189) break;
 *   }
 */

/**
 * 用 BigInt 计算单个斐波那契数 F(n)
 * 使用快速倍增法（Fast Doubling），时间复杂度 O(log n)
 *
 * 递推公式:
 *   F(2k)   = F(k) * [ 2*F(k+1) - F(k) ]
 *   F(2k+1) = F(k)^2 + F(k+1)^2
 */
export function fibonacciAt(n: number): bigint {
    if (n < 0) {
        throw new RangeError(`fibonacciAt: n 必须 >= 0，收到 ${n}`);
    }
    if (n === 0) return 0n;
    if (n === 1) return 1n;

    // 快速倍增法，返回 (F(k), F(k+1))
    function fibPair(k: number): [bigint, bigint] {
        if (k === 0) return [0n, 1n];
        const [a, b] = fibPair(k >> 1); // F(k), F(k+1)
        const c = a * (2n * b - a);     // F(2k)
        const d = a * a + b * b;        // F(2k+1)
        if (k & 1) {
            return [d, c + d];           // F(2k+1), F(2k+2)
        }
        return [c, d];                   // F(2k), F(2k+1)
    }

    return fibPair(n)[0];
}

/**
 * 生成前 190 项斐波那契数列（数组）
 * F(0) = 0, F(1) = 1, ..., F(189)
 */
export function fibonacciSequence(): bigint[] {
    const result: bigint[] = new Array(190);
    result[0] = 0n;
    result[1] = 1n;
    for (let i = 2; i < 190; i++) {
        result[i] = result[i - 1] + result[i - 2];
    }
    return result;
}

/**
 * 斐波那契数列生成器（惰性迭代器）
 * 每次 yield [序号, 值]，从 F(0) 开始
 */
export function* fibonacci(): Generator<[number, bigint], void, unknown> {
    let a = 0n;
    let b = 1n;
    let i = 0;
    yield [i++, a];
    yield [i++, b];
    while (true) {
        [a, b] = [b, a + b];
        yield [i++, b];
    }
}

/**
 * 打印前 190 项斐波那契数列（格式化输出）
 */
export function printFibonacci190(): string {
    const seq = fibonacciSequence();
    const lines: string[] = [];
    lines.push("=== 斐波那契数列前 190 项 ===\n");
    for (let i = 0; i < seq.length; i++) {
        const digits = seq[i].toString().length;
        lines.push(`F(${String(i).padStart(3)}) = ${seq[i]}  (${digits} 位)`);
    }
    lines.push(`\n最大项 F(189) 有 ${seq[189].toString().length} 位十进制数字`);
    return lines.join("\n");
}

// 直接运行时打印前 190 项
if (require.main === module) {
    console.log(printFibonacci190());
}
