export type ProbeFn = (host: string, httpPort: number) => Promise<{ serverId: string } | null>;
