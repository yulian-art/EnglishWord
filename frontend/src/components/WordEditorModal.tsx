import { Alert, App as AntApp, Button, Col, Form, Input, Modal, Row, Typography } from 'antd';
import { useMutation } from '@tanstack/react-query';
import { api, type Word } from '../lib/api';
import { parseMeanings, trimWordText, wordKey, wordTextError } from '../lib/words';

export type WordEditorTarget = {
  bookId: number;
  bookName: string;
} & ({ mode: 'add' } | { mode: 'edit' | 'meaning'; word: Word });

interface FormValues {
  english: string;
  partOfSpeech?: string;
  meanings?: string;
  meaning?: string;
}

interface Props {
  target: WordEditorTarget;
  words: Word[];
  open: boolean;
  onClose: () => void;
  afterClose: () => void;
  onSaved: (word: Word, bookId: number) => void;
  onSupplement: (word: Word) => void;
}

const TITLES = { add: '添加单词', edit: '修改单词', meaning: '补充释义' };
const ACTIONS = { add: '添加', edit: '保存修改', meaning: '补充释义' };

export default function WordEditorModal({
  target,
  words,
  open,
  onClose,
  afterClose,
  onSaved,
  onSupplement,
}: Props) {
  const [form] = Form.useForm<FormValues>();
  const { message } = AntApp.useApp();
  const word = target.mode === 'add' ? undefined : target.word;
  const english = Form.useWatch('english', form) as string | undefined;
  const duplicate =
    target.mode === 'add' && english
      ? words.find((item) => wordKey(item.english) === wordKey(english))
      : undefined;

  const save = useMutation({
    mutationFn: (values: FormValues) => {
      if (target.mode === 'meaning')
        return api.addMeaning(target.word.id, trimWordText(values.meaning ?? ''));
      const fields = {
        english: trimWordText(values.english),
        partOfSpeech: trimWordText(values.partOfSpeech ?? ''),
      };
      return target.mode === 'edit'
        ? api.updateWord(target.word.id, fields)
        : api.addWord(target.bookId, { ...fields, meanings: parseMeanings(values.meanings ?? '') });
    },
    onSuccess: (saved) => {
      onSaved(saved, target.bookId);
      if (target.mode === 'meaning' && saved.meanings.length === target.word.meanings.length) {
        message.info('这条释义已存在，未重复添加');
      } else {
        message.success(
          target.mode === 'meaning'
            ? `已为 ${saved.english} 补充释义`
            : target.mode === 'edit'
              ? `已保存 ${saved.english} 的修改`
              : `已添加 ${saved.english}`,
        );
      }
      onClose();
    },
  });

  return (
    <Modal
      title={`${TITLES[target.mode]} · ${target.bookName}`}
      open={open}
      onCancel={() => {
        if (!save.isPending) onClose();
      }}
      afterClose={afterClose}
      onOk={() => form.submit()}
      okText={ACTIONS[target.mode]}
      cancelText="取消"
      confirmLoading={save.isPending}
      cancelButtonProps={{ disabled: save.isPending }}
      closable={!save.isPending}
      maskClosable={!save.isPending}
      keyboard={!save.isPending}
      destroyOnHidden
    >
      {save.isError ? (
        <Alert
          className="modal-hint"
          type="error"
          showIcon
          title="保存未成功"
          description={save.error.message}
        />
      ) : null}
      {word ? (
        <div className="word-editor-summary">
          <div className="word-editor-heading">
            <strong>{word.english}</strong>
            {word.partOfSpeech ? <span>{word.partOfSpeech}</span> : null}
          </div>
          <Typography.Text type="secondary">已有释义 · {word.meanings.length} 条</Typography.Text>
          <ol className="word-meanings-list">
            {word.meanings.map((meaning, index) => (
              <li key={index}>{meaning}</li>
            ))}
          </ol>
        </div>
      ) : null}
      <Form<FormValues>
        form={form}
        name="word-editor"
        layout="vertical"
        preserve={false}
        disabled={save.isPending}
        initialValues={{ english: word?.english ?? '', partOfSpeech: word?.partOfSpeech ?? '' }}
        onFinish={(values) => {
          if (!save.isPending) save.mutate(values);
        }}
      >
        {target.mode !== 'meaning' ? (
          <>
            <Row gutter={16}>
              <Col xs={24} sm={14}>
                <Form.Item
                  label="英文单词"
                  name="english"
                  rules={[
                    {
                      validator: (_rule, value = '') => {
                        const error =
                          wordTextError(value, '英文单词', 100) ??
                          (!/[A-Za-z]/.test(value) ? '单词至少包含一个英文字母' : undefined) ??
                          (words.some(
                            (item) =>
                              item.id !== word?.id && wordKey(item.english) === wordKey(value),
                          )
                            ? '该单词已存在，请为已有单词补充释义'
                            : undefined);
                        return error ? Promise.reject(new Error(error)) : Promise.resolve();
                      },
                    },
                  ]}
                >
                  <Input
                    placeholder="例如：apple"
                    autoFocus
                    autoComplete="off"
                    autoCapitalize="none"
                    autoCorrect="off"
                    spellCheck={false}
                  />
                </Form.Item>
              </Col>
              <Col xs={24} sm={10}>
                <Form.Item
                  label="词性"
                  name="partOfSpeech"
                  rules={[
                    {
                      validator: (_rule, value = '') => {
                        const error = wordTextError(value, '词性', 50, false);
                        return error ? Promise.reject(new Error(error)) : Promise.resolve();
                      },
                    },
                  ]}
                >
                  <Input placeholder="例如：n.（选填）" autoComplete="off" />
                </Form.Item>
              </Col>
            </Row>
            {duplicate ? (
              <Alert
                className="modal-hint"
                type="info"
                showIcon
                title={`词库中已有 ${duplicate.english}`}
                description="要增加新的中文含义，可以直接为这个单词补充释义。"
                action={
                  <Button size="small" onClick={() => onSupplement(duplicate)}>
                    去补充释义
                  </Button>
                }
              />
            ) : null}
            {target.mode === 'add' ? (
              <Form.Item
                label="中文释义"
                name="meanings"
                extra="每行一条释义，空行和重复释义会自动忽略。"
                rules={[
                  {
                    validator: (_rule, value = '') => {
                      const meanings = parseMeanings(value);
                      const error = !meanings.length
                        ? '请至少填写一条释义'
                        : meanings.length > 50
                          ? '一个单词最多填写 50 条不同释义'
                          : meanings
                              .map((meaning) => wordTextError(meaning, '每条释义', 500))
                              .find(Boolean);
                      return error ? Promise.reject(new Error(error)) : Promise.resolve();
                    },
                  },
                ]}
              >
                <Input.TextArea rows={3} placeholder={'例如：苹果\n苹果树'} />
              </Form.Item>
            ) : (
              <Typography.Paragraph type="secondary" className="page-footnote">
                修改英文或词性后，已有释义会保留。需要增加含义时，请使用「补充释义」。
              </Typography.Paragraph>
            )}
          </>
        ) : (
          <Form.Item
            label="新的中文释义"
            name="meaning"
            extra="每次补充一条，已有释义会保留，最多 50 条。"
            rules={[
              {
                validator: (_rule, value = '') => {
                  const error =
                    wordTextError(value, '新的中文释义', 500) ??
                    (word?.meanings.some((meaning) => wordKey(meaning) === wordKey(value))
                      ? '这条释义已存在，请填写新的释义'
                      : undefined) ??
                    (word && word.meanings.length >= 50
                      ? '该单词已有 50 条释义，无法继续补充'
                      : undefined);
                  return error ? Promise.reject(new Error(error)) : Promise.resolve();
                },
              },
            ]}
          >
            <Input.TextArea rows={3} autoFocus placeholder="例如：苹果树" />
          </Form.Item>
        )}
      </Form>
    </Modal>
  );
}
