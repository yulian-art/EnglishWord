/** 与词库 API 的 ASCII 空白及大小写去重规则一致。 */
export function trimWordText(value: string): string {
  return value.replace(/^[ \t\r\n\f\v]+|[ \t\r\n\f\v]+$/g, '');
}

export function wordKey(value: string): string {
  return trimWordText(value).replace(/[A-Z]/g, (letter) => letter.toLowerCase());
}

/** 新增单词时逐行收集释义，忽略空行与重复释义。 */
export function parseMeanings(value: string): string[] {
  const meanings = new Map<string, string>();
  for (const line of value.split(/\r?\n/)) {
    const meaning = trimWordText(line);
    if (meaning && !meanings.has(wordKey(meaning))) meanings.set(wordKey(meaning), meaning);
  }
  return [...meanings.values()];
}

export function wordTextError(
  value: string,
  label: string,
  max: number,
  required = true,
): string | undefined {
  const text = trimWordText(value);
  if (required && !text) return `请输入${label}`;
  if ([...text].length > max) return `${label}不超过 ${max} 个字符`;
  if (/[\u0000-\u001f\u007f-\u009f]/.test(text))
    return `${label}不能包含制表符、换行或其他控制字符`;
  return undefined;
}
