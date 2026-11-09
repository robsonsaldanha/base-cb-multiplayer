/** Detalhe de falha de um serviço externo, seguro para log (nunca tokens, códigos ou segredos). */
export interface UpstreamInfo {
  service: string;
  endpoint: string;
  status?: number;
  error?: string;
}

export class HttpError extends Error {
  constructor(readonly status: number, readonly code: string, message: string, readonly upstream?: UpstreamInfo) {
    super(message);
  }
}
