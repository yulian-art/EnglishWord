import { useState } from 'react';
import {
  Alert,
  App,
  Button,
  Card,
  Form,
  Input,
  Progress,
  Radio,
  Result,
  Select,
  Space,
  Statistic,
  Tag,
  Typography,
} from 'antd';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { Link, useSearchParams } from 'react-router-dom';
import { api, ApiError, type StudyMode, type StudyState } from '../lib/api';
const modeLabels: Record<StudyMode, string> = {
  choice: '选择题',
  spelling: '拼写单词',
  meaning: '看英文输中文',
};

const modePrompts: Record<StudyMode, string> = {
  choice: '请选择中文对应的英文单词',
  spelling: '请写出中文对应的英文单词',
  meaning: '请写出英文单词的中文释义',
};
export default function StudyPage({ review = false }: { review?: boolean }) {
  const { message } = App.useApp();
  const client = useQueryClient();
  const [answer, setAnswer] = useState('');
  const [selectedBook, setSelectedBook] = useState<number>();
  const [mode, setMode] = useState<StudyMode>('spelling');
  const books = useQuery({ queryKey: ['wordbooks'], queryFn: api.wordbooks });
  const study = useQuery({ queryKey: ['study'], queryFn: api.study, staleTime: 0 });
  const state = study.data;
  const [searchParams] = useSearchParams();
  const value = Number(searchParams.get('wordbookId'));
  const urlBookId = Number.isSafeInteger(value) && value > 0 ? value : undefined;
  const bookId = selectedBook ?? urlBookId ?? books.data?.items[0]?.id;
  const mutation = useMutation({
    mutationFn: (action: () => Promise<StudyState>) => action(),
    onSuccess: (data) => {
      client.setQueryData(['study'], data);
      if (!data.result) {
        setAnswer('');
      }
    },
    onError: (error: Error) => {
      void message.error(error.message);
    },
  });
  const noSession = study.error instanceof ApiError && study.error.status === 404;
  const busy = mutation.isPending;
  function handleAnswerSubmit() {
    if (!state || state.finished || state.canAdvance || busy) {
      return;
    }
    const submittedAnswer = answer.trim();
    if (!submittedAnswer) {
      message.warning('请先输入答案');
      return;
    }

    mutation.mutate(() => api.answer(submittedAnswer));
  }

  async function handleRetry() {
    await books.refetch();
  }
  return (
    <Space orientation="vertical" size={20} className="full-width">
      <div>
        <Typography.Title level={2}>{review ? '错题复习' : '开始背诵'}</Typography.Title>
        <Typography.Text type="secondary">
          {review
            ? '完成一轮后，把答错的单词再认真练习一遍。'
            : '看中文选英文、看中文拼写，或看英文输中文。'}
        </Typography.Text>
      </div>
      {study.isPending && <Card loading />}
      {study.isError && !noSession && (
        <Alert type="error" showIcon title="背诵状态加载失败" description={study.error.message} />
      )}
      {books.isError && (
        <Alert
          type="error"
          showIcon
          title="词库加载失败"
          description={books.error.message}
          action={
            <Button loading={books.isFetched} onClick={() => void handleRetry()}>
              重新加载
            </Button>
          }
        />
      )}

      {(!state || state.finished) && !review && (
        <Card title={state?.finished ? '再开始一轮' : '准备好了吗？'}>
          <Form
            layout="vertical"
            onFinish={() => {
              if (bookId !== undefined) mutation.mutate(() => api.start(bookId, mode));
            }}
          >
            <Form.Item label="选择词库">
              <Select
                aria-label="选择词库"
                value={bookId}
                loading={books.isPending}
                onChange={setSelectedBook}
                options={books.data?.items.map((book) => ({
                  value: book.id,
                  label: `${book.name}（${book.wordCount} 词）`,
                }))}
              />
            </Form.Item>
            <Form.Item label="背诵模式">
              <Radio.Group
                value={mode}
                onChange={(event) => setMode(event.target.value)}
                optionType="button"
                options={[
                  { label: '拼写单词', value: 'spelling' },
                  { label: '选择题', value: 'choice' },
                  { label: '看英文输中文', value: 'meaning' },
                ]}
              />
            </Form.Item>
            <Button
              type="primary"
              htmlType="submit"
              loading={busy}
              disabled={bookId === undefined || study.isPending}
            >
              开始背诵
            </Button>
          </Form>
        </Card>
      )}

      {review && noSession && (
        <Card>
          <Result
            status="info"
            title="还没有本轮错题"
            subTitle="先完成一轮背诵，再来这里加强记忆。"
            extra={
              <Link to="/study">
                <Button type="primary">去背诵</Button>
              </Link>
            }
          />
        </Card>
      )}
      {review && state && !state.finished && (
        <Alert type="info" showIcon title="请先完成当前背诵，再开始错题复习" />
      )}

      {state && !state.finished && (
        <Card className="study-card">
          <div className="study-meta">
            <Tag color="green">{modeLabels[state.mode]}</Tag>
            <Typography.Text>
              第 {state.currentIndex + 1} / {state.totalCount} 题 · 当前 {state.score} 分
            </Typography.Text>
          </div>
          <Progress
            percent={Math.round((state.currentIndex / state.totalCount) * 100)}
            showInfo={false}
          />
          <Typography.Paragraph type="secondary">{modePrompts[state.mode]}</Typography.Paragraph>
          <Typography.Title level={2}>
            {state.mode === 'meaning'
              ? state.question?.english
              : state.question?.meanings.join('；')}
          </Typography.Title>
          <Typography.Paragraph type="secondary">
            {state.question?.partOfSpeech}
          </Typography.Paragraph>
          <Form onFinish={handleAnswerSubmit}>
            <Form.Item>
              {state.mode === 'choice' ? (
                <Radio.Group
                  value={answer}
                  onChange={(event) => setAnswer(event.target.value)}
                  disabled={busy || state.canAdvance}
                  className="choice-options"
                  options={state.question?.choices.map((option) => ({
                    label: option,
                    value: option,
                  }))}
                />
              ) : (
                <Input
                  aria-label={state.mode === 'meaning' ? '中文答案' : '英文答案'}
                  size="large"
                  placeholder={state.mode === 'meaning' ? '输入中文释义' : '输入英文单词'}
                  autoComplete="off"
                  spellCheck={false}
                  value={answer}
                  onChange={(event) => setAnswer(event.target.value)}
                  disabled={busy || state.canAdvance}
                />
              )}
            </Form.Item>
            {state.result && (
              <Alert
                className="answer-feedback"
                showIcon
                type={state.result.correct ? 'success' : 'warning'}
                title={state.result.message}
                description={`本次 ${state.result.scoreDelta > 0 ? '+' : ''}${state.result.scoreDelta} 分；即时重试不重复计分。`}
              />
            )}
            <Space wrap>
              <Button
                type="primary"
                htmlType="submit"
                loading={busy}
                disabled={!answer.trim() || state.canAdvance}
              >
                {state.canAdvance ? '已答对' : '提交答案'}
              </Button>
              <Button
                disabled={!state.canAdvance || busy}
                onClick={() => mutation.mutate(api.next)}
              >
                {state.currentIndex + 1 === state.totalCount ? '完成本轮' : '下一题'}
              </Button>
            </Space>
          </Form>
        </Card>
      )}

      {state?.finished && (
        <Card>
          <Result
            status={state.wrongCount ? 'info' : 'success'}
            title={state.wrongCount ? '本轮完成，再巩固一下错词' : '没有错题，继续努力！'}
            subTitle={`首次答对 ${state.correctCount} / ${state.totalCount} 题，首次正确率 ${Math.round((state.correctCount / state.totalCount) * 100)}%。`}
            extra={
              <Space>
                <Statistic title="本轮得分" value={state.score} />
                <Statistic title="本轮错词" value={state.wrongCount} />
              </Space>
            }
          />
          {state.wrongCount > 0 && (
            <Button type="primary" block loading={busy} onClick={() => mutation.mutate(api.review)}>
              错题重背（拼写模式）
            </Button>
          )}
        </Card>
      )}
      <Typography.Text type="secondary">
        访客学习任务由 Cookie
        会话暂存，最长闲置一小时；重启后端会清空。历史成绩与跨轮次错题本待实现。
      </Typography.Text>
    </Space>
  );
}
