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
  sessionId?: number | null;
  wordbookId?: number;
  mode: StudyMode;
  score: number;
  correctCount: number;
  totalCount: number;
  currentIndex: number;
  wrongCount: number;
  finished: boolean;
  canAdvance: boolean;
  accuracy?: number;
  passed?: boolean;
  passThreshold?: number;
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

/** 历史成绩条目，对应 GET /api/history。 */
export interface StudyHistoryEntry {
  id: number;
  wordbookId: number | null;
  wordbookName: string;
  mode: StudyMode;
  score: number;
  totalCount: number;
  correctCount: number;
  firstAttemptCount: number;
  accuracy: number;
  passThreshold: number;
  passed: boolean;
  finished: boolean;
  startedAt: string;
  finishedAt: string | null;
}

/** 累计统计，对应 GET /api/statistics。 */
export interface Statistics {
  totalScore: number;
  sessionCount: number;
  completedSessionCount: number;
  firstAttemptCount: number;
  correctCount: number;
  accuracy: number;
  passThreshold: number;
  passed: boolean;
}

export interface CheckinState {
  dates: string[];
  totalDays: number;
}

export interface CheckinResult {
  date: string;
  alreadyCheckedIn: boolean;
  totalDays: number;
}

export interface ImportResult {
  imported: number;
  merged: number;
  skipped: number;
}

export class ApiError extends Error {
  constructor(
    message: string,
    public readonly status: number,
  ) {
    super(message);
    this.name = 'ApiError';
  }
}

/**
 * 使用同源 Cookie 访问 Drogon API，非成功响应统一抛出可展示的错误。
 * 后端错误统一为 {"error": "..."}；这里额外容忍空响应和非 JSON 响应，避免解析异常掩盖真实状态码。
 */
async function request<T>(
  path: string,
  data?: unknown,
  method: 'GET' | 'POST' | 'PATCH' | 'DELETE' = data === undefined ? 'GET' : 'POST',
): Promise<T> {
  let response: Response;
  try {
    response = await fetch(`/api${path}`, {
      method,
      credentials: 'same-origin',
      headers: method === 'GET' ? undefined : { 'Content-Type': 'application/json' },
      body: data === undefined ? undefined : JSON.stringify(data),
      signal: AbortSignal.timeout(20_000),
    });
  } catch (error) {
    throw new ApiError(
      error instanceof DOMException && error.name === 'TimeoutError'
        ? '请求超时，请检查网络后重试。'
        : '暂时无法连接服务，请检查网络后重试。',
      0,
    );
  }
  const raw = await response.text();
  let result: unknown = null;
  if (raw) {
    try {
      result = JSON.parse(raw);
    } catch {
      if (response.ok) throw new ApiError('服务器返回了无法解析的响应。', response.status);
    }
  }
  if (!response.ok) {
    const details = result as {
      error?: string;
      errors?: { line: number; message: string }[];
    } | null;
    const message =
      details?.error ??
      (response.status >= 500 ? '服务暂时不可用，请稍后重试。' : `请求失败：${response.status}`);
    const lines = details?.errors
      ?.slice(0, 3)
      .map((item) => `第 ${item.line} 行：${item.message}`)
      .join('；');
    throw new ApiError(lines ? `${message}：${lines}` : message, response.status);
  }
  return result as T;
}

export const api = {
  health: () => request<{ status: string; mode: string; framework: string }>('/health'),
  wordbooks: () => request<{ items: Wordbook[] }>('/wordbooks'),
  createWordBook: (name: string) => request<Wordbook>('/wordbooks', { name }),
  words: (id: number) => request<{ items: Word[] }>(`/wordbooks/${id}/words`),
  importTsv: (id: number, content: string, duplicatePolicy: 'merge' | 'skip') =>
    request<ImportResult>(`/wordbooks/${id}/import`, { content, duplicatePolicy }),
  study: () => request<StudyState>('/study'),
  start: (wordbookId: number, mode: StudyMode) =>
    request<StudyState>('/study/start', { wordbookId, mode }),
  answer: (answer: string) => request<StudyState>('/study/answer', { answer }),
  next: () => request<StudyState>('/study/next', {}),
  review: () => request<StudyState>('/study/review', {}),
  history: () => request<{ items: StudyHistoryEntry[] }>('/history'),
  statistics: () => request<Statistics>('/statistics'),
  checkins: () => request<CheckinState>('/checkins'),
  checkIn: () => request<CheckinResult>('/checkins', {}),
  register: (username: string, password: string) =>
    request<AuthResponse>('/auth/register', { username, password }),
  login: (username: string, password: string) =>
    request<AuthResponse>('/auth/login', { username, password }),
  logout: (checkin = false) =>
    request<{ success: boolean; checkin?: CheckinResult }>('/auth/logout', { checkin }),
  me: () => request<AuthResponse>('/auth/me'),
  addWord: (bookId: number, word: Omit<Word, 'id'>) =>
    request<Word>(`/wordbooks/${bookId}/words`, word),
  updateWord: (wordId: number, word: Pick<Word, 'english' | 'partOfSpeech'>) =>
    request<Word>(`/words/${wordId}`, word, 'PATCH'),
  addMeaning: (wordId: number, meaning: string) =>
    request<Word>(`/words/${wordId}/meanings`, { meaning }),
  deleteWord: (wordId: number) =>
    request<{ success: boolean }>(`/words/${wordId}`, undefined, 'DELETE'),
};
