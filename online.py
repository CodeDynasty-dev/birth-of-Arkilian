import torch
import torch.nn as nn
import torch.nn.functional as F

class OnlineSemanticNetwork(nn.Module):

    def __init__(self, input_dim=384, hidden_dim=128, output_dim=128, lr=0.01):
        super().__init__()

        # frozen random projection
        self.W1 = nn.Parameter(
            torch.randn(input_dim, hidden_dim) * 0.1,
            requires_grad=False
        )

        # trainable projection
        self.W2 = nn.Parameter(
            torch.randn(hidden_dim, output_dim) * 0.1
        )

        self.lr = lr

    def forward(self, x):
        h = torch.relu(x @ self.W1)
        y = h @ self.W2
        return F.normalize(y, dim=-1)

    def online_update(self, query_vec, target_vec):

        q = torch.tensor(query_vec, dtype=torch.float32).unsqueeze(0)
        t = torch.tensor(target_vec, dtype=torch.float32).unsqueeze(0)

        pred = self.forward(q)

        loss = -F.cosine_similarity(pred, t).mean()

        loss.backward()

        with torch.no_grad():
            self.W2 -= self.lr * self.W2.grad
            self.W2.grad.zero_()

        return float(loss)