import { useMemo, useState } from 'react';
import {
  Alert,
  App as AntApp,
  Button,
  Card,
  Dropdown,
  Empty,
  Form,
  Grid,
  Input,
  Modal,
  Radio,
  Select,
  Space,
  Table,
  Tag,
  Typography,
} from 'antd';
import type { ColumnsType } from 'antd/es/table';
import {
  DeleteOutlined,
  DownloadOutlined,
  EditOutlined,
  EllipsisOutlined,
  FileAddOutlined,
  LoginOutlined,
  PlusOutlined,
  ReadOutlined,
  SearchOutlined,
  UploadOutlined,
} from '@ant-design/icons';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { useNavigate } from 'react-router-dom';
import { api, type Word } from '../lib/api';
import { useCurrentUser } from '../lib/queries';
import PageHeader from '../components/PageHeader';
import { ErrorState } from '../components/PageStates';
import WordEditorModal, { type WordEditorTarget } from '../components/WordEditorModal';

interface CreateBookValues {
  name: string;
}

interface ImportValues {
  content: string;
  duplicatePolicy: 'merge' | 'skip';
}

/** 把字符串内容作为 TSV 文件下载到本地（带 BOM，Excel 打开不乱码）。 */
function downloadTsv(content: string, filename: string) {
  const blob = new Blob(['\uFEFF', content], {
    type: 'text/tab-separated-values;charset=utf-8',
  });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}

export default function WordbooksPage() {
  const navigate = useNavigate();
  const queryClient = useQueryClient();
  const { message, modal } = AntApp.useApp();
  const screens = Grid.useBreakpoint();
  const compact = screens.md === false;
  const auth = useCurrentUser();
  const user = auth.data;

  const [selected, setSelected] = useState<number>();
  const [keyword, setKeyword] = useState('');
  const [page, setPage] = useState(1);
  const [createOpen, setCreateOpen] = useState(false);
  const [editorTarget, setEditorTarget] = useState<WordEditorTarget | null>(null);
  const [editorOpen, setEditorOpen] = useState(false);
  const [importOpen, setImportOpen] = useState(false);

  const [createForm] = Form.useForm<CreateBookValues>();
  const [importForm] = Form.useForm<ImportValues>();

  const books = useQuery({ queryKey: ['wordbooks'], queryFn: api.wordbooks });
  const bookId = books.data?.items.some((book) => book.id === selected)
    ? selected
    : books.data?.items[0]?.id;
  const currentBook = books.data?.items.find((book) => book.id === bookId);
  const words = useQuery({
    queryKey: ['words', bookId],
    queryFn: () => api.words(bookId!),
    enabled: bookId !== undefined,
  });

  const query = keyword.trim().toLowerCase();
  const allWords = words.data?.items ?? [];
  const filteredWords = useMemo(() => {
    if (!query) return allWords;
    return allWords.filter(
      (word) =>
        word.english.toLowerCase().includes(query) ||
        word.meanings.some((meaning) => meaning.toLowerCase().includes(query)),
    );
  }, [allWords, query]);

  const canEdit = Boolean(user) && Boolean(currentBook) && !currentBook?.isDemo;
  const canStart = bookId !== undefined && allWords.length > 0;

  const createBook = useMutation({
    mutationFn: (name: string) => api.createWordBook(name),
    onSuccess: (book) => {
      setCreateOpen(false);
      setSelected(book.id);
      setKeyword('');
      setPage(1);
      queryClient.setQueryData(['words', book.id], { items: [] });
      message.success(`已创建词库「${book.name}」`);
      void queryClient.invalidateQueries({ queryKey: ['wordbooks'] });
    },
    onError: (error: Error) => message.error(error.message),
  });

  const deleteWord = useMutation({
    mutationFn: ({ word }: { word: Word; bookId: number }) => api.deleteWord(word.id),
    onSuccess: (_result, { word, bookId: targetBookId }) => {
      message.success(`已删除 ${word.english}`);
      queryClient.setQueryData<{ items: Word[] }>(['words', targetBookId], (data) =>
        data ? { items: data.items.filter((item) => item.id !== word.id) } : data,
      );
      void queryClient.invalidateQueries({ queryKey: ['words', targetBookId] });
      void queryClient.invalidateQueries({ queryKey: ['wordbooks'] });
    },
    onError: (error: Error) => message.error(error.message),
  });

  function openEditor(mode: 'add' | 'edit' | 'meaning', word?: Word) {
    if (!canEdit || !currentBook) return;
    const book = { bookId: currentBook.id, bookName: currentBook.name };
    if (mode === 'add') setEditorTarget({ ...book, mode });
    else if (word) setEditorTarget({ ...book, mode, word });
    else return;
    setEditorOpen(true);
  }

  function handleWordSaved(word: Word, targetBookId: number) {
    queryClient.setQueryData<{ items: Word[] }>(['words', targetBookId], (data) => {
      if (!data) return data;
      return {
        items: data.items.some((item) => item.id === word.id)
          ? data.items.map((item) => (item.id === word.id ? word : item))
          : [...data.items, word],
      };
    });
    void queryClient.invalidateQueries({ queryKey: ['words', targetBookId] });
    void queryClient.invalidateQueries({ queryKey: ['wordbooks'] });
  }

  function confirmDelete(word: Word) {
    if (!canEdit || !currentBook || deleteWord.isPending) return;
    const targetBookId = currentBook.id;
    modal.confirm({
      title: `删除单词「${word.english}」？`,
      content: `将从「${currentBook.name}」移除这个单词及其 ${word.meanings.length} 条释义，删除后无法恢复。`,
      okText: '确认删除',
      cancelText: '取消',
      okButtonProps: { danger: true },
      onOk: () => deleteWord.mutateAsync({ word, bookId: targetBookId }),
    });
  }

  const importWords = useMutation({
    mutationFn: (values: ImportValues) =>
      api.importTsv(bookId!, values.content, values.duplicatePolicy),
    onSuccess: (result) => {
      setImportOpen(false);
      message.success(
        `导入完成：新增 ${result.imported} 条，补全 ${result.merged} 条，跳过 ${result.skipped} 条`,
      );
      void queryClient.invalidateQueries({ queryKey: ['words', bookId] });
      void queryClient.invalidateQueries({ queryKey: ['wordbooks'] });
    },
    // 保留弹窗和输入，逐行校验信息在表单内展示。
  });

  function handleExport() {
    const rows = filteredWords.flatMap((word) => {
      const meanings = word.meanings.length > 0 ? word.meanings : [''];
      return meanings.map((meaning) => [word.english, word.partOfSpeech, meaning].join('\t'));
    });
    const content = ['english\tpos\tmeaning', ...rows].join('\n');
    const safeName = (currentBook?.name ?? 'wordbook').replace(/[\\/:*?"<>|\s]+/g, '-');
    downloadTsv(content, `${safeName}.tsv`);
  }

  const columns: ColumnsType<Word> = [
    {
      title: '英文单词',
      dataIndex: 'english',
      width: compact ? 148 : 220,
      render: (value: string, word) => (
        <div className="word-cell">
          <span className="word-english">{value}</span>
          {compact && word.partOfSpeech ? (
            <Tag bordered={false} className="pos-tag">
              {word.partOfSpeech}
            </Tag>
          ) : null}
        </div>
      ),
    },
  ];

  if (!compact) {
    columns.push({
      title: '词性',
      dataIndex: 'partOfSpeech',
      width: 110,
      render: (value: string) =>
        value ? (
          <Tag bordered={false} className="pos-tag">
            {value}
          </Tag>
        ) : (
          <Typography.Text type="secondary">—</Typography.Text>
        ),
    });
  }

  columns.push({
    title: '中文释义',
    dataIndex: 'meanings',
    render: (values: string[]) => <span className="meaning-text">{values.join('；')}</span>,
  });

  if (canEdit) {
    columns.push({
      title: '操作',
      key: 'action',
      width: compact ? 64 : 268,
      align: 'right',
      render: (_value, word) =>
        compact ? (
          <Dropdown
            trigger={['click']}
            menu={{
              items: [
                { key: 'edit', label: '修改单词', icon: <EditOutlined /> },
                { key: 'meaning', label: '补充释义', icon: <FileAddOutlined /> },
                { type: 'divider' },
                { key: 'delete', label: '删除单词', icon: <DeleteOutlined />, danger: true },
              ],
              onClick: ({ key }) =>
                key === 'delete'
                  ? confirmDelete(word)
                  : openEditor(key as 'edit' | 'meaning', word),
            }}
          >
            <Button
              type="text"
              icon={<EllipsisOutlined />}
              aria-label={`维护 ${word.english}`}
              disabled={deleteWord.isPending}
            />
          </Dropdown>
        ) : (
          <Space size={0} className="word-row-actions">
            <Button
              type="link"
              size="small"
              icon={<EditOutlined />}
              onClick={() => openEditor('edit', word)}
              disabled={deleteWord.isPending}
              aria-label={`修改 ${word.english}`}
            >
              修改
            </Button>
            <Button
              type="link"
              size="small"
              icon={<FileAddOutlined />}
              onClick={() => openEditor('meaning', word)}
              disabled={deleteWord.isPending}
              aria-label={`为 ${word.english} 补充释义`}
            >
              补充释义
            </Button>
            <Button
              type="text"
              size="small"
              icon={<DeleteOutlined />}
              className="row-action-danger"
              loading={deleteWord.isPending && deleteWord.variables?.word.id === word.id}
              disabled={deleteWord.isPending && deleteWord.variables?.word.id !== word.id}
              onClick={() => confirmDelete(word)}
              aria-label={`删除 ${word.english}`}
            >
              删除
            </Button>
          </Space>
        ),
    });
  }

  return (
    <div className="page-stack">
      <PageHeader
        title="我的词库"
        description="先认识单词，再开始今天的练习。示例词库只读，登录后可以创建并维护自己的词库。"
        extra={
          <Space wrap>
            <Button
              icon={<DownloadOutlined />}
              onClick={handleExport}
              disabled={!filteredWords.length}
              title="导出当前筛选结果（UTF-8 TSV）"
            >
              导出 TSV
            </Button>
            {user ? (
              <Button type="primary" icon={<PlusOutlined />} onClick={() => setCreateOpen(true)}>
                新建词库
              </Button>
            ) : (
              <Button type="primary" icon={<LoginOutlined />} onClick={() => navigate('/login')}>
                登录后新建词库
              </Button>
            )}
          </Space>
        }
      />

      {auth.isError ? (
        <ErrorState
          title="账号状态加载失败"
          error={auth.error}
          onRetry={() => void auth.refetch()}
          retrying={auth.isFetching}
        />
      ) : null}

      {books.isError ? (
        <ErrorState
          title="词库列表加载失败"
          error={books.error}
          onRetry={() => void books.refetch()}
          retrying={books.isFetching}
        />
      ) : null}
      {words.isError ? (
        <ErrorState
          title="单词列表加载失败"
          error={words.error}
          onRetry={() => void words.refetch()}
          retrying={words.isFetching}
        />
      ) : null}

      <Card className="surface-card">
        <div className="table-toolbar">
          <div className="table-toolbar-left">
            <Select
              aria-label="选择词库"
              className="book-select"
              value={bookId}
              loading={books.isPending}
              placeholder="选择词库"
              onChange={(value) => {
                setSelected(value);
                setKeyword('');
                setPage(1);
              }}
              options={books.data?.items.map((book) => ({
                value: book.id,
                label: `${book.name}（${book.wordCount} 词）`,
              }))}
            />
            {currentBook ? (
              currentBook.isDemo ? (
                <Tag bordered={false} color="gold">
                  示例词库 · 只读
                </Tag>
              ) : (
                <Tag bordered={false} color="green">
                  私人词库
                </Tag>
              )
            ) : null}
          </div>
          <div className="table-toolbar-right">
            <Input
              aria-label="搜索单词"
              className="search-input"
              placeholder="搜索英文或中文释义"
              allowClear
              prefix={<SearchOutlined />}
              value={keyword}
              onChange={(event) => {
                setKeyword(event.target.value);
                setPage(1);
              }}
            />
            <Button
              icon={<UploadOutlined />}
              disabled={!canEdit}
              onClick={() => {
                importWords.reset();
                setImportOpen(true);
              }}
              title={canEdit ? undefined : '仅私人词库支持导入'}
            >
              导入 TSV
            </Button>
            <Button
              icon={<PlusOutlined />}
              disabled={!canEdit}
              onClick={() => openEditor('add')}
              title={canEdit ? undefined : '仅私人词库支持添加单词'}
            >
              添加单词
            </Button>
            <Button
              type="primary"
              icon={<ReadOutlined />}
              disabled={!canStart}
              onClick={() => navigate(`/study?wordbookId=${bookId}`)}
            >
              开始背诵
            </Button>
          </div>
        </div>

        <div className="table-summary">
          <Typography.Text type="secondary">
            共 {allWords.length} 个单词
            {query ? `，匹配 ${filteredWords.length} 个` : ''}
          </Typography.Text>
          {currentBook?.isDemo ? (
            <Typography.Text type="secondary">
              示例词库为公共只读数据，登录后可导入自己的词库。
            </Typography.Text>
          ) : null}
        </div>

        <Table<Word>
          rowKey="id"
          size={compact ? 'small' : 'middle'}
          loading={books.isPending || words.isFetching}
          dataSource={filteredWords}
          columns={columns}
          pagination={{
            current: Math.min(
              page,
              Math.max(1, Math.ceil(filteredWords.length / (compact ? 8 : 10))),
            ),
            onChange: setPage,
            pageSize: compact ? 8 : 10,
            hideOnSinglePage: true,
            showSizeChanger: false,
            size: compact ? 'small' : undefined,
          }}
          scroll={{ x: compact ? (canEdit ? 340 : 300) : canEdit ? 800 : 600 }}
          locale={{
            emptyText:
              books.isError || words.isError ? (
                <Empty
                  image={Empty.PRESENTED_IMAGE_SIMPLE}
                  description="数据未能加载，请使用上方按钮重试"
                />
              ) : query ? (
                <Empty
                  image={Empty.PRESENTED_IMAGE_SIMPLE}
                  description={`没有匹配「${keyword.trim()}」的单词`}
                >
                  <Button
                    onClick={() => {
                      setKeyword('');
                      setPage(1);
                    }}
                  >
                    清除搜索
                  </Button>
                </Empty>
              ) : (
                <Empty
                  image={Empty.PRESENTED_IMAGE_SIMPLE}
                  description={
                    !bookId ? '还没有可用的词库' : canEdit ? '这本词库还没有单词' : '暂时没有单词'
                  }
                >
                  {canEdit ? (
                    <Space>
                      <Button type="primary" onClick={() => openEditor('add')}>
                        添加单词
                      </Button>
                      <Button
                        onClick={() => {
                          importWords.reset();
                          setImportOpen(true);
                        }}
                      >
                        导入 TSV
                      </Button>
                    </Space>
                  ) : null}
                </Empty>
              ),
          }}
        />
      </Card>

      <Alert
        type="info"
        showIcon
        title="可以做什么"
        description="选择私人词库后，可以新增单词、修改英文与词性、为已有单词补充新的释义，也可以删除单词或批量导入 TSV。修改与补充都会保留已有释义；示例词库仅供浏览和练习。"
      />

      <Modal
        title="新建词库"
        open={createOpen}
        onCancel={() => {
          if (!createBook.isPending) setCreateOpen(false);
        }}
        cancelButtonProps={{ disabled: createBook.isPending }}
        closable={!createBook.isPending}
        maskClosable={!createBook.isPending}
        keyboard={!createBook.isPending}
        onOk={() => createForm.submit()}
        okText="创建"
        cancelText="取消"
        confirmLoading={createBook.isPending}
        destroyOnHidden
      >
        <Form<CreateBookValues>
          form={createForm}
          layout="vertical"
          preserve={false}
          disabled={createBook.isPending}
          onFinish={(values) => createBook.mutate(values.name.trim())}
        >
          <Form.Item
            label="词库名称"
            name="name"
            rules={[
              { required: true, whitespace: true, message: '请输入词库名称' },
              { max: 40, message: '名称不超过 40 个字符' },
            ]}
          >
            <Input placeholder="例如：四级核心词" maxLength={40} showCount autoFocus />
          </Form.Item>
        </Form>
      </Modal>

      {editorTarget ? (
        <WordEditorModal
          key={`${editorTarget.bookId}:${editorTarget.mode}:${editorTarget.mode === 'add' ? 'new' : editorTarget.word.id}`}
          target={editorTarget}
          words={allWords}
          open={editorOpen}
          onClose={() => setEditorOpen(false)}
          afterClose={() => setEditorTarget(null)}
          onSaved={handleWordSaved}
          onSupplement={(word) => openEditor('meaning', word)}
        />
      ) : null}

      <Modal
        title={`导入 TSV${currentBook ? `：${currentBook.name}` : ''}`}
        open={importOpen}
        onCancel={() => {
          if (!importWords.isPending) setImportOpen(false);
        }}
        cancelButtonProps={{ disabled: importWords.isPending }}
        closable={!importWords.isPending}
        maskClosable={!importWords.isPending}
        keyboard={!importWords.isPending}
        onOk={() => importForm.submit()}
        okText="开始导入"
        cancelText="取消"
        width={640}
        confirmLoading={importWords.isPending}
        destroyOnHidden
      >
        {importWords.isError ? (
          <Alert
            className="modal-hint"
            type="error"
            showIcon
            title="导入未成功"
            description={importWords.error.message}
          />
        ) : null}
        <Alert
          className="modal-hint"
          type="info"
          showIcon
          title="格式：english<Tab>pos<Tab>meaning"
          description="每行一个释义，可带表头。全部行校验通过才会写入；任一行出错会整批回滚，不会留下半成品。"
        />
        <Form<ImportValues>
          form={importForm}
          layout="vertical"
          preserve={false}
          disabled={importWords.isPending}
          initialValues={{ duplicatePolicy: 'merge' }}
          onFinish={(values) => importWords.mutate(values)}
        >
          <Form.Item
            label="TSV 内容"
            name="content"
            rules={[
              { required: true, whitespace: true, message: '请粘贴 TSV 内容' },
              {
                validator: (_rule, value: string) =>
                  !value || new TextEncoder().encode(value).length <= 48 * 1024
                    ? Promise.resolve()
                    : Promise.reject(new Error('TSV 内容不能超过 48 KiB，请拆分后导入')),
              },
            ]}
          >
            <Input.TextArea
              className="code-area"
              rows={9}
              spellCheck={false}
              placeholder={'english\tpos\tmeaning\napple\tn.\t苹果\napple\tn.\t苹果树'}
            />
          </Form.Item>
          <Form.Item label="遇到已存在的单词" name="duplicatePolicy">
            <Radio.Group
              optionType="button"
              options={[
                { label: '补充新释义', value: 'merge' },
                { label: '跳过已有词', value: 'skip' },
              ]}
            />
          </Form.Item>
        </Form>
      </Modal>
    </div>
  );
}
