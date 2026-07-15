import axios from 'axios';

const api = axios.create({
  baseURL: '/api',
  headers: { 'Content-Type': 'application/json' },
  timeout: 10000,
});

api.interceptors.response.use(
  (res) => res,
  (err) => {
    const message = err.response?.data?.error || err.message || 'Unknown error';
    return Promise.reject(new Error(message));
  }
);

export const submitTask = (task) => api.post('/tasks', task);
export const submitTaskPriority = (task) => api.post('/tasks/priority', task);
export const scheduleTask = (task) => api.post('/tasks/schedule', task);
export const cancelTask = (id) => api.delete(`/tasks/${id}`);

export const getWorkers = () => api.get('/workers');
export const getQueue = () => api.get('/queue');
export const getMetrics = () => api.get('/metrics');
export const getHealth = () => api.get('/health');
export const shutdown = () => api.post('/shutdown');

export default api;
