export interface User {
  id: number;
  username: string;
  createdAt: string;
}

export interface AuthResponse {
  user: User;
}
export interface Wordbook {
  id: number;
  name: string;
  wordCount: number;
  isDemo: boolean;
}

export interface Word {
  id: number;
  english: string;
  partOfSpeech: string;
  meanings: string[];
}

export type StudyMode = 'choice' | 'spelling' | 'meaning';

export interface StudyState {
  mode: StudyMode;
  score: number;
  correctCount: number;
  totalCount: number;
  currentIndex: number;
  wrongCount: number;
  finished: boolean;
  canAdvance: boolean;
  question: {
    wordId: number;
    partOfSpeech: string;
    english?: string;
    meanings: string[];
    choices: string[];
  } | null;
  result?: {
    correct: boolean;
    firstAttempt: boolean;
    scoreDelta: number;
    message: string;
  };
}

export class ApiError extends Error {
  constructor(
    message: string,
    public readonly status: number,
  ) {
    super(message);
  }
}

/** 使用同源 Cookie 访问 Drogon API，非成功响应统一抛出可展示的错误。 */
async function request<T>(
  path: string,
  data?: unknown,
  method: 'GET' | 'POST' | 'PATCH' | 'DELETE' = data === undefined ? 'GET' : 'POST',
): Promise<T> {
  const response = await fetch(`/api${path}`, {
    method,
    credentials: 'same-origin',
    headers: method === 'GET' ? undefined : { 'Content-Type': 'application/json' },
    body: data === undefined ? undefined : JSON.stringify(data),
  });
  const result = await response.json();
  if (!response.ok) {
    throw new ApiError(result.error ?? `请求失败：${response.status}`, response.status);
  }
  return result as T;
}

export const api = {
  health: () => request<{ status: string; mode: string; framework: string }>('/health'),
  wordbooks: () => request<{ items: Wordbook[] }>('/wordbooks'),
  createWordBook: (name: string) => request<Wordbook>('/wordbooks', { name }),
  words: (id: number) => request<{ items: Word[] }>(`/wordbooks/${id}/words`),
  study: () => request<StudyState>('/study'),
  start: (wordbookId: number, mode: StudyMode) =>
    request<StudyState>('/study/start', { wordbookId, mode }),
  answer: (answer: string) => request<StudyState>('/study/answer', { answer }),
  next: () => request<StudyState>('/study/next', {}),
  review: () => request<StudyState>('/study/review', {}),
  register: (username: string, password: string) =>
    request<AuthResponse>('/auth/register', { username, password }),
  login: (username: string, password: string) =>
    request<AuthResponse>('/auth/login', { username, password }),
  logout: () => request<{ success: boolean }>('/auth/logout', {}),
  me: () => request<AuthResponse>('/auth/me'),
  addWord: (bookId: number, word: Omit<Word, 'id'>) =>
    request<Word>(`/wordbooks/${bookId}/words`, word),
  deleteWord: (wordId: number) =>
    request<{ success: boolean }>(`/words/${wordId}`, undefined, 'DELETE'),
};
