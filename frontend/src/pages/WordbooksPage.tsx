import { useState } from 'react';
import {
  Alert,
  Button,
  Card,
  Select,
  Space,
  Table,
  Tag,
  Typography,
  Input,
  Modal,
  Form,
} from 'antd';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { App as AntApp } from 'antd';
import { Link } from 'react-router-dom';
import { api, type Word } from '../lib/api';

export default function WordbooksPage() {
  const [createOpen, setCreateOpen] = useState(false);
  const [selected, setSelected] = useState<number>();
  const books = useQuery({ queryKey: ['wordbooks'], queryFn: api.wordbooks });
  const bookId = selected ?? books.data?.items[0]?.id;
  const words = useQuery({
    queryKey: ['words', bookId],
    queryFn: () => api.words(bookId!),
    enabled: bookId !== undefined,
  });
  const [keyword, setKeyword] = useState('');
  const query = keyword.trim().toLowerCase();
  const filteredWords = (words.data?.items ?? []).filter((word) => {
    const matchesEnglish = word.english.toLowerCase().includes(query);
    const matchesChinese = word.meanings.some((meaning) => meaning.includes(query));

    return matchesEnglish || matchesChinese;
  });
  const canStart = bookId !== undefined && words.isSuccess && words.data.items.length > 0;
  const [addWordOpen, setAddWordOpen] = useState(false);
  const currentBook = books.data?.items.find((book) => book.id === bookId);

  /**
   * 将字符串内容作为 TSV 文件下载到本地
   * @param content 要写入 TSV 文件的字符串内容（通常为多行、制表符分隔的文本）
   */
  function downloadTsv(content: string) {
    // 1. 创建 Blob：把字符串内容包装成二进制大对象
    //    - '\uFEFF' 是 UTF-8 BOM（字节序标记），放在内容最前面
    //      作用是让 Excel 等软件识别出"这是 UTF-8 编码"，避免打开时中文乱码
    //    - type 指定 MIME 类型为 TSV，并声明字符集为 utf-8
    const blob = new Blob(['\uFEFF', content], {
      type: 'text/tab-separated-values;charset=utf-8',
    });

    // 2. 为 Blob 生成一个临时的、当前页面内有效的 URL（形如 blob:https://xxx/uuid）
    const url = URL.createObjectURL(blob);

    // 3. 创建一个隐藏的 <a> 标签，用来模拟用户点击下载
    const link = document.createElement('a');
    link.href = url; // 指向上面生成的 Blob URL
    link.download = 'wordbook.tsv'; // 指定下载后的文件名（浏览器会以此名保存）

    // 4. 将 <a> 插入 DOM
    //    Firefox 等浏览器要求元素必须在文档中，click() 才能触发下载
    document.body.appendChild(link);

    // 5. 以编程方式触发点击，等价于用户点击了这个链接，开始下载
    link.click();

    // 6. 下载触发后，把 <a> 从 DOM 中移除，避免残留无用节点
    link.remove();

    // 7. 释放之前创建的 Blob URL，回收内存（否则该 URL 会一直占用内存直到页面关闭）
    URL.revokeObjectURL(url);
  }

  function handleExport() {
    const rows = filteredWords.flatMap((word) =>
      word.meanings.map((meaning) => [word.english, word.partOfSpeech, meaning].join('\t')),
    );
    const content = ['english\tpos\tmeaning', ...rows].join('\n');

    downloadTsv(content);
  }
  const queryClient = useQueryClient();
  const { message } = AntApp.useApp();

  const createMutation = useMutation({
    mutationFn: (name: string) => api.createWordBook(name),
    onSuccess: () => {
      setCreateOpen(false);
      message.success('词库创建成功');
      queryClient.invalidateQueries({ queryKey: ['wordbooks'] });
    },
    onError: (error: Error) => {
      message.error(error.message);
    },
  });
  const addWordMutation = useMutation({
    mutationFn: (data: { bookId: number; word: Omit<Word, 'id'> }) =>
      api.addWord(data.bookId, data.word),
    onSuccess: (_word, data) => {
      setAddWordOpen(false);
      setKeyword('');
      message.success('单词添加成功');
      queryClient.invalidateQueries({
        queryKey: ['words', data.bookId],
      });
      queryClient.invalidateQueries({
        queryKey: ['wordbooks'],
      });
    },
    onError: (error: Error) => {
      message.error(error.message);
    },
  });

  return (
    <Space orientation="vertical" size={20} className="full-width">
      <Modal title="新建词库" open={createOpen} onCancel={() => setCreateOpen(false)} footer={null}>
        <Form
          layout="vertical"
          onFinish={(values: { name: string }) => {
            createMutation.mutate(values.name);
          }}
        >
          <Form.Item
            label="词库名称"
            name="name"
            rules={[{ required: true, message: '请输入词库名称' }]}
          >
            <Input placeholder="例如：四级词库" />
          </Form.Item>
          <Button type="primary" htmlType="submit" loading={createMutation.isPending}>
            创建
          </Button>
        </Form>
      </Modal>
      <Modal
        title={`添加单词：${currentBook?.name ?? ''}`}
        open={addWordOpen}
        onCancel={() => setAddWordOpen(false)}
        footer={null}
      >
        <Form
          layout="vertical"
          onFinish={(values: { english: string; partOfSpeech: string; meaning: string }) => {
            const word = {
              english: values.english,
              partOfSpeech: values.partOfSpeech,
              meanings: [values.meaning],
            };

            if (bookId !== undefined) {
              addWordMutation.mutate({ bookId, word });
            }
          }}
        >
          <Form.Item
            label="英文单词"
            name="english"
            rules={[{ required: true, message: '请输入英文单词' }]}
          >
            <Input placeholder="例如：apple" />
          </Form.Item>
          <Form.Item
            label="词性"
            name="partOfSpeech"
            rules={[{ required: true, message: '请输入词性' }]}
          >
            <Input placeholder="例如：n." />
          </Form.Item>
          <Form.Item
            label="中文释义"
            name="meaning"
            rules={[{ required: true, message: '请输入中文释义' }]}
          >
            <Input placeholder="例如：苹果" />
          </Form.Item>
          <Button type="primary" htmlType="submit" loading={addWordMutation.isPending}>
            添加
          </Button>
        </Form>
      </Modal>
      <div>
        <Typography.Title level={2}>我的词库</Typography.Title>
        <Typography.Text type="secondary">先认识单词，再开始今天的练习。</Typography.Text>
      </div>
      <Alert
        type="info"
        showIcon
        title="当前展示公共示例词库"
        description="私人词库、文件导入、增删改和补充释义的开发任务见 docs/PLAN.md。"
      />
      {(books.isError || words.isError) && (
        <Alert
          type="error"
          showIcon
          title="词库加载失败"
          description={(books.error ?? words.error)?.message}
        />
      )}
      <Card>
        <Space wrap className="toolbar">
          <Button
            disabled={!currentBook || currentBook.isDemo}
            onClick={() => setAddWordOpen(true)}
          >
            添加单词
          </Button>
          <Button type="primary" onClick={() => setCreateOpen(true)}>
            新建词库
          </Button>
          <Select
            aria-label="选择词库"
            style={{ minWidth: 220 }}
            value={bookId}
            loading={books.isPending}
            onChange={(value) => {
              setSelected(value);
              setKeyword('');
            }}
            options={books.data?.items.map((book) => ({
              value: book.id,
              label: `${book.name}（${book.wordCount} 词）`,
            }))}
          />
          <Button onClick={handleExport}>导出当前结果</Button>
          {canStart ? (
            <Link to={`/study?wordbookId=${bookId}`}>
              <Button type="primary">开始背诵</Button>
            </Link>
          ) : (
            <Button type="primary" disabled>
              开始背诵
            </Button>
          )}
          <Tag>只读示例</Tag>
        </Space>
        <Input
          aria-label="搜索单词"
          placeholder="搜索英文或中文释义"
          allowClear
          value={keyword}
          onChange={(event) => setKeyword(event.target.value)}
          style={{ marginBottom: 16 }}
        />
        {words.isSuccess && (
          <Typography.Paragraph type="secondary">
            共 {words.data.items.length} 个单词， 匹配 {filteredWords.length} 个
          </Typography.Paragraph>
        )}
        <Table<Word>
          rowKey="id"
          loading={words.isFetching}
          dataSource={filteredWords}
          pagination={{ pageSize: 10 }}
          scroll={{ x: 560 }}
          columns={[
            {
              title: '英文单词',
              dataIndex: 'english',
              render: (value: string) => <strong className="word-english">{value}</strong>,
            },
            { title: '词性', dataIndex: 'partOfSpeech', width: 100 },
            {
              title: '中文释义',
              dataIndex: 'meanings',
              render: (values: string[]) => values.join('；'),
            },
          ]}
        />
      </Card>
    </Space>
  );
}
