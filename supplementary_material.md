# Appendix C
# Instantiating the Hash Function


Theorem 1 assumes that the hash function $h$ is drawn from a $q$-wise independent family; our implementation uses a pseudorandom function instead.
This changes only the failure probability: the trace does not depend on $h$ (Lemma 1), Lemmas 1, 2 and 4 to 9 hold for every fixed $h$, and only Lemma 3 depends on how $h$ is chosen.
For a truly random function, relocations can be half as frequent under the same condition on $B$, and a pseudorandom function adds only its distinguishing advantage.

### C.1. A Truly Random Function

In this appendix, we allow every integer $a$ with $1 \leq a \leq B/4$ as the number of operations between two relocations, instead of only $a = B/8$.
The proofs in Sections A.1 and A.2 use the value of $a$ only through $W_d = 2^d a$ and in the bounds $|E_\rho| \leq B/2 + a < B$ and $|D_\rho| \leq a < B/2$ in the proof of Lemma 9, which hold for every $a < B/2$, so their results hold for every such $a$; the restriction $a \leq B/4$ is used only in (3) below.

Let $u$, $t$, $A$, $I_k$ and $X = \sum_{k \in A} I_k$ be as in the proof of Lemma 3, and let $\vartheta := B/2$ if $u$ is internal and $\vartheta := B+1$ if $u$ is a leaf, so that $u$ is overloaded at time $t$ if and only if $X \geq \vartheta$.
The first part of that proof only uses that every single value $h(k)$ is uniform, so it also applies to truly random functions, whose values $h(k)$ for distinct keys are independent and uniform over $\{0,1\}^L$, and gives $\mathbb E[X] \leq a \leq B/4$ if $u$ is internal and $\mathbb E[X] \leq B/4$ if $u$ is a leaf, and hence

\[\mathbb E[X] \leq \frac{\vartheta}{2}. \qquad (3)\]

Now let $h$ be a truly random function.
Then the indicators $I_k$ are independent, and instead of counting $q$-tuples we can use the Chernoff bound [36]
\[
\Pr[X \geq \vartheta] \leq e^{-\mathbb E[X]} \left( \frac{e \cdot \mathbb E[X]}{\vartheta} \right)^\vartheta \quad \text{for } \vartheta > \mathbb E[X].
\]
If $\mathbb E[X] = 0$, then $X = 0$, so we may assume $\mathbb E[X] > 0$.
The right-hand side $\phi(y) := e^{-y}(ey/\vartheta)^\vartheta$, with $y = \mathbb E[X]$, satisfies $\phi'(y) = \phi(y)(\vartheta/y - 1) > 0$ for $0 < y < \vartheta$, so we may replace $\mathbb E[X]$ by any upper bound $\mu < \vartheta$.

##### Lemma 13. No overload for a random function
Fix an operation sequence $S$ as in Section 3.3, let $1 \leq a \leq B/4$, and let $h$ be a truly random function.
Then
\[
\Pr\nolimits_h\!\left[
  \begin{gathered}
    \text{some node is overloaded}\\
    \text{at some time } t \in \{1,\dots,N\}
  \end{gathered}
\right] \leq N |T|\,2^{-B/8}.
\]

*Proof.* With $\mu = \vartheta/2$ from (3), we have $e\mu/\vartheta = e/2$, and therefore
\[
\Pr[X \geq \vartheta]
\leq e^{-\vartheta/2} \left( \frac{e}{2} \right)^{\vartheta}
= e^{-(\ln 2 - 1/2)\,\vartheta}
\leq 2^{-\vartheta/4}
\leq 2^{-B/8},
\]
since $(\ln 2 - \frac12)/\ln 2 = 1 - \frac{1}{2\ln 2} > 0.27 > \frac14$ and $\vartheta \geq B/2$.
A union bound over the $|T|$ nodes and the $N$ times gives the claim. $\square$


For $a = B/8$, the first part of the proof of Lemma 3 gives $\mathbb E[X] \leq \vartheta/4$, and $\mu = \vartheta/4$ gives $e^{-(\ln 4 - 3/4)\,\vartheta} \leq 2^{-0.9\,\vartheta}$, since $(\ln 4 - \frac34)/\ln 2 = 2 - \frac{3}{4\ln 2} > 0.9$; so the bound improves to $N|T|\,2^{-0.45B}$.
Replacing Lemma 3 by Lemma 13 in the proof of Theorem 1 gives the same condition on $B$ for every $a \leq B/4$, and the amortized cost becomes $12L/a$ I/Os; the client memory no longer contains a description of $h$, which we treat as an oracle.

##### Theorem 3.
Let $\delta \in (0,1)$, let $h$ be a truly random function, let $B \geq 8\lceil\log(16Nn/\delta)\rceil$ be divisible by eight, let $n \geq 2B$, and let $1 \leq a \leq B/4$ be an integer.
Then the construction of Figure 1, with relocations every $a$ operations, is a data-independent and $\delta$-correct priority queue implementation.
Every operation costs at most $12L$ I/Os in the worst case and at most $12L/a$ I/Os amortized, which is $48L/B$ for $a = B/4$; the server memory is less than $24n$ records and, not counting the description of $h$, the client memory is $O(B)$ records.


### C.2. A Pseudorandom Function


A truly random function cannot be stored, so the implementation uses a pseudorandom function $F$ with a secret key $\kappa$ and outputs of at least $L$ bits, and sets $h(k) := F_\kappa(k)$, truncated to $L$ bits.
For an algorithm $\mathcal D$ with oracle access to a function, let
\[
\mathsf{Adv}_F(\mathcal D) := \big| \Pr_\kappa[\mathcal D^{F_\kappa} = 1] - \Pr_g[\mathcal D^{g} = 1] \big|,
\]
where $\kappa$ is a uniformly random key and $g$ is a truly random function with the same domain and range as $F_\kappa$.
If $F$ is a pseudorandom permutation, such as AES, the PRP/PRF switching lemma [37] bounds this advantage by the advantage against $F$ as a permutation plus $\nu^2/2^{b+1}$, where $\nu$ is the number of oracle queries and $b$ the block length.

Fix an operation sequence $S$ of length at most $N$ with at most $n$ present keys at all times.
Consider the following distinguisher $\mathcal D_S$.
It is given oracle access to a function $g$, runs our construction on $S$ with $h(k) := g(k)$ truncated to $L$ bits, and computes the correct answers of $S$ itself.
It outputs $1$ if some answer of the construction differs from the correct answer, and $0$ otherwise.
If $g = F_\kappa$, then $\mathcal D_S$ outputs $1$ exactly when some answer of our construction on $S$ is incorrect.
If $g$ is truly random, then its values truncated to $L$ bits are independent and uniform over $\{0,1\}^L$, so $h$ is a truly random function, and $\mathcal D_S$ outputs $1$ with probability at most $N|T|\,2^{-B/8}$ by Lemmas 2 and 13.
By the definition of the advantage,
\[
\Pr\nolimits_\kappa\!\left[
  \begin{gathered}\text{some answer on}\\ S \text{ is incorrect} 
\end{gathered}\right]  \leq N |T| \, 2^{-B/8} + \mathsf{Adv}_F(\mathcal D_S).
\]

The distinguisher runs in the time of executing $S$ plus computing the correct answers, so if this time is polynomial in the security parameter, its advantage is negligible for a pseudorandom function that is secure against non-uniform distinguishers, since $S$ is built into $\mathcal D_S$.
This gives the following corollary of Theorem 3.

##### Corollary 2.
Let $h$ be $F_\kappa$ truncated to $L$ bits for a pseudorandom function $F$, as above, and let $\delta$, $B$, $n$ and $a$ be as in Theorem 3.
Then the construction is data-independent and has the cost and memory bounds of Theorem 3, with the key $\kappa$ in place of the description of $h$.
Moreover, for every operation sequence $S$ of length at most $N$ with at most $n$ present keys at all times,
\[
\Pr\nolimits_\kappa[\text{some answer on } S \text{ is incorrect}] \leq \delta + \mathsf{Adv}_F(\mathcal D_S).
\]

###### Remark 2. (Spread relocations)
The proof of Lemma 13 uses only (3), which the proof of Lemma 12 also establishes for late overloads, since $3B/16 \leq B/4$ (and $|A| \leq 2a = B/4$ at the root).
So for a truly random function, the construction of Section 3.5 fails with probability at most $N|T|\,2^{-B/8}$, which is at most $\delta$ under the conditions of Theorem 2, and for the pseudorandom $h$ of Corollary 2 with probability at most $\delta + \mathsf{Adv}_F(\mathcal D_S)$, where $\mathcal D_S$ runs this construction.


With a pseudorandom function, only correctness becomes computational, and only against operation sequences that are chosen without knowledge of $\kappa$.
As we show next, this guarantee extends to operation sequences chosen adaptively by a process that sees the answers and the trace but neither $\kappa$ nor the ciphertexts.

### C.3. Adaptively Chosen Operations

Theorems 1 and 3 and Corollary 2 assume that the operation sequence is fixed before the hash function $h$ is chosen.
In applications, later operations often depend on earlier answers; Dijkstra's algorithm, for example, inserts the neighbors of every vertex it extracts.
The guarantees remain valid as long as the operations are chosen without knowledge of $h$.

##### Corollary 3.
Let $\delta$, $q$, $B$ and $n$ be as in Theorem 1, and let the operations be chosen one by one by a randomized process that sees all answers and the trace, but neither $h$ nor the ciphertexts, and whose random choices are independent of $h$.
Suppose that, whatever answers it receives, the process issues at most $N$ operations and never has more than $n$ keys present.
Then all answers are correct with probability at least $1-\delta$.


*Proof.* Fix the random choices of the process.
Let $S$ be the sequence of operations that the process issues if it always receives the correct answers.
By Lemma 1, the trace it sees after $m$ operations is $f(m)$ in any case, so $S$ does not depend on $h$.
With our queue, the process receives exactly the same answers and traces as long as all answers are correct, so it issues the operations of $S$ up to the first operation that is answered incorrectly.
Hence it receives an incorrect answer if and only if some answer of our queue on the fixed sequence $S$ is incorrect, which by Theorem 1 happens with probability at most $\delta$.
Since this holds for every fixed choice of the randomness of the process, it also holds on average over it. $\square$


The proof of Corollary 3 applies to the construction of Section 3.5, whose trace is also fixed, under the conditions of Theorem 2, to the construction of Section 3 with a truly random function under the conditions of Theorem 3, and to the pseudorandom $h$ of Corollary 2 if the process sees neither $\kappa$ nor any value of $F_\kappa$ and its random choices are independent of $\kappa$.
In the last case, the failure probability becomes $\delta + \mathsf{Adv}_F(\mathcal D')$ for the distinguisher $\mathcal D'$ that samples the random choices of the process, simulates the process with the correct answers to obtain $S$, and then behaves as $\mathcal D_S$, which is efficient if the process is.
The ciphertexts are excluded because their contents depend on $h$; a process that also sees them, such as a server that chooses the operations, can be handled by first replacing them with encryptions of fixed strings, at the price of the advantage against the encryption scheme, which we do not formalize.

### C.4. Concrete Parameters

We now illustrate the bounds with concrete numbers, for $n = 2^{23}$ present keys and $N = 2^{24}$ operations, which covers our experiments.
For a buffer size $B$, the tree has depth $L = \lceil \log(2^{25}/B) \rceil$ and $|T| = 2^{L+1} - 1$ nodes, so the number of pairs $(u,t)$ in the union bound is $N|T| < 2^{L+25}$.
The following table lists the resulting bounds on the failure probability:
$N|T|\,3^{-B/8}$ for a $q$-wise independent function with the largest admissible $q = B/8$ (Lemma 3), and, for a truly random function, $N|T|\,2^{-0.45B}$ for $a = B/8$ (by the paragraph after Lemma 13) and $N|T|\,2^{-B/8}$ for $a = B/4$ (Lemma 13).
Exponents are rounded towards zero, a dash means that the bound exceeds $1$, and the bold entries are the smallest multiples of eight for which these bounds are at most $\delta = 2^{-30}$.

| $B$ | $L$ | $N$ | q-wise, $a=B/8$ | Random function, $a=B/8$ | Random function, $a=B/4$ |
|---:|---:|---:|---:|---:|---:|
| 168  | 18 | $2^{43}$ | —       | **$2^{-32}$**  | —        |
| 368  | 17 | $2^{42}$ | $2^{-30}$ | $2^{-123}$   | $2^{-4}$ |
| 568  | 16 | $2^{41}$ | $2^{-71}$ | $2^{-214}$   | **$2^{-30}$** |
| 1024 | 15 | $2^{40}$ | $2^{-162}$ | $2^{-420}$ | $2^{-88}$ |
| 2048 | 14 | $2^{39}$ | $2^{-366}$ | $2^{-882}$ | $2^{-217}$ |

*The last three columns give upper bounds on the failure probability.*
<!-- 
\begin{center}
\setlength{\tabcolsep}{6pt}
\renewcommand{\arraystretch}{1.2}
\begin{tabular}{@{}rccrrr@{}}
\toprule
& & & \multicolumn{3}{c}{failure probability at most} \\
\cmidrule(l){4-6}
& & & $q$-wise & \multicolumn{2}{c}{random function} \\
\cmidrule(l){4-4} \cmidrule(l){5-6}
$B$ & $L$ & $N\abs{T} <$ & $a = B/8$ & $a = B/8$ & $a = B/4$ \\
\midrule
$168$ & $18$ & $2^{43}$ & -- & $\mathbf{2^{-32}}$ & -- \\
$368$ & $17$ & $2^{42}$ & $\mathbf{2^{-30}}$ & $2^{-123}$ & $2^{-4}$ \\
$568$ & $16$ & $2^{41}$ & $2^{-71}$ & $2^{-214}$ & $\mathbf{2^{-30}}$ \\
$1024$ & $15$ & $2^{40}$ & $2^{-162}$ & $2^{-420}$ & $2^{-88}$ \\
$2048$ & $14$ & $2^{39}$ & $2^{-366}$ & $2^{-882}$ & $2^{-217}$ \\
\bottomrule
\end{tabular}
\end{center} -->

The random-function column for $a = B/8$ concerns the construction of Section 3; for the construction of Section 3.5, which also uses $a = B/8$, Remark 2 gives only the bound of the column $a = B/4$.
For $\delta = 2^{-30}$, the bound of Lemma 3 is at most $\delta$ if $\frac B8 \log 3 \geq L + 55$, first for $B = 368$ ($L = 17$).
For a truly random function, the bound is at most $\delta$ if $0.45B \geq L + 55$ for $a = B/8$, first for $B = 168$ ($L = 18$), and if $B/8 \geq L + 55$ for $a = B/4$, first for $B = 568$ ($L = 16$).

The conditions of Theorems 1 and 3 avoid the dependence of $L$ on $B$, and that of Theorem 1 uses $2^{-q}$ instead of $3^{-q}$, so they are more conservative: $\lceil \log(16Nn/\delta) \rceil = 81$, and both ask for $B \geq 648$ ($B \geq 416$ for Theorem 1 with $\log_3$).
So a random function reaches the same failure probability with less than half the buffer size, or, from $B = 568$ on, with relocations half as frequent as for $a = B/8$.
In all cases, the client memory of $O(B)$ records is a tiny fraction of the $2^{23}$ outsourced keys.

All these buffer sizes, from $168$ to $648$, are small.
Since $B$ counts records, a bucket with $B = 368$ and $3$-byte keys and priorities, consisting of $B$ pairs and $B/2$ markers, occupies about $2.8$~KB, which is less than a typical disk page.
Larger buffers remain admissible and reduce the number of relocations, at the price of more client memory and more data transferred by a single relocation.
In practice, the buffer size is therefore dictated by the memory budget of the client and by performance, not by correctness.



# Appendix D
# Additional Measurements


|  | TEE average | TEE slowest | RTT 1 ms average | RTT 1 ms slowest | RTT 10 ms average | RTT 10 ms slowest | RTT 50 ms average | RTT 50 ms slowest |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| JLS [3] | 5.4 μs | 2.1 s | 0.16 ms | 1.4 min | 1.2 ms | 11 min | 6.2 ms | 58 min |
| PRQ-Stream | 4.3 μs | 2.0 ms | 0.14 ms | 36 ms | 1.2 ms | 297 ms | 5.9 ms | 1.5 s |
| PRQ-Batch | 3.5 μs | 1.0 ms | 22 μs | 5.7 ms | 57 μs | 15 ms | 0.34 ms | 88 ms |
| PRQ-Spread | 7.9 μs | 0.31 ms | 0.28 ms | 1.5 ms | 2.3 ms | 10 ms | 12 ms | 52 ms |
| Path Heap [5] | 56 μs | 2.0 ms | 1.2 ms | 3.1 ms | 10 ms | 12 ms | 51 ms | 53 ms |

**Table 5.** Average time per operation and time of the slowest single operation, for $n=2^{23}$ and $B=1024$, in all four environments (the values of Figure 2). Path Heap, below the rule, is only oblivious.


Table 5 lists the average and the slowest operation of every queue in all four environments.
The three network environments differ mainly in scale: the speed-ups over Path Heap are $8.6$ to $8.8$ for PRQ-Stream and $4.3$ to $4.4$ for PRQ-Spread at $1$, $10$ and $50$ ms, and only those of PRQ-Batch, which transfers a whole path in one round trip, depend on the RTT.


Here we discuss the effect of the buffer size for $n = 2^{23}$ at an RTT of $50~$ms.
A larger $B$ makes relocations rarer and hence the average time smaller, roughly in proportion to $1/B$: from $26~$ms for $B = 256$ to $1.4~$ms for $B = 4096$ with PRQ-Stream.
The slowest operation of PRQ-Stream barely changes, from $1.7$ to $1.4~$s, since its $2L-1$ round trips shrink only with $\log\frac nB$, whereas that of PRQ-Batch grows with the size of the path it transfers, from $66$ to $180~$ms.
PRQ-Spread needs $\lceil 2L/a \rceil = 2$ round trips per operation for $B = 256$ and one for larger $B$, which doubles its slowest operation for $B = 256$, to $101~$ms.
The slowest operation of JLS shrinks with $B$ only because the tree has fewer nodes, from $3.7~$h to $17~$min.
The client memory is proportional to $B$: $6B$ records for JLS and PRQ-Stream, $55B/8$ for PRQ-Spread and $3BL + B/2$ for PRQ-Batch, from $13\,184$ to $161\,792$ records.
