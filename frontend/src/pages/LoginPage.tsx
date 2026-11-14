import { useState } from 'react';
import { App as AntApp, Button, Card, Form, Input, Segmented, Space, Typography } from 'antd';
import { useMutation, useQueryClient } from '@tanstack/react-query';
import { useNavigate } from 'react-router-dom';
import { api } from '../lib/api';

type Mode = 'login' | 'register';

interface FormValues {
  username: string;
  password: string;
}

export default function LoginPage() {
  const [mode, setMode] = useState<Mode>('login'); // 当前在登录还是注册。
  const navigate = useNavigate();
  const queryClient = useQueryClient();
  const { message } = AntApp.useApp(); // 全局提示。

  const mutation = useMutation({
    mutationFn: async (values: FormValues) => {
      if (mode === 'register') {
        await api.register(values.username, values.password); // 先注册。
      }
      return api.login(values.username, values.password); // 注册后直接登录，少一步。
    },
    onSuccess: async (data) => {
      await queryClient.invalidateQueries({ queryKey: ['me'] }); // 让“我是谁”重新拉一次。
      message.success(`欢迎，${data.user.username}`);
      navigate('/');
    },
    onError: (error: Error) => {
      message.error(error.message); // 后端返回的 {"error":"..."} 会在这里显示。
    },
  });

  return (
    <Space orientation="vertical" size={20} className="full-width">
      <Typography.Title level={2}>账号</Typography.Title>
      <Card style={{ maxWidth: 460 }}>
        <Segmented
          block
          value={mode}
          onChange={(value) => setMode(value as Mode)}
          options={[
            { label: '登录', value: 'login' },
            { label: '注册', value: 'register' },
          ]}
        />
        <Form<FormValues>
          layout="vertical"
          onFinish={(values) => mutation.mutate(values)}
          style={{ marginTop: 20 }}
        >
          <Form.Item
            name="username"
            label="用户名"
            rules={[{ required: true, message: '请输入用户名' }]}
          >
            <Input autoComplete="username" placeholder="3-32 位字母、数字或下划线" />
          </Form.Item>
          <Form.Item
            name="password"
            label="密码"
            rules={[{ required: true, message: '请输入密码' }]}
          >
            <Input.Password
              autoComplete={mode === 'login' ? 'current-password' : 'new-password'}
              placeholder="至少 8 位"
            />
          </Form.Item>
          <Button type="primary" htmlType="submit" loading={mutation.isPending} block>
            {mode === 'login' ? '登录' : '注册并登录'}
          </Button>
        </Form>
      </Card>
    </Space>
  );
}
