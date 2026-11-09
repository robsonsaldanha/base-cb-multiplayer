export interface DiscordUser { id: string; username: string; global_name: string | null; avatar: string | null }
export interface DiscordClient {
  exchangeCode(code: string, redirectUri: string): Promise<string>;
  getUser(accessToken: string): Promise<DiscordUser>;
}
