// nfs5-web: no movie decoding in the page (nfsu2-web's AV1 path). PU's few movies are not part of the base data.
export const movieCodec = async (_env: Record<string, string>): Promise<string> => 'none';
export const movieOpen = (..._a: unknown[]): number => 52;   // ENOSYS
export const movieDecode = async (..._a: unknown[]): Promise<number> => 52;
export const movieClose = (..._a: unknown[]): number => 0;
