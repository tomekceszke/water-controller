"""Estimators with one interface (fit(X, y, w) / predict(X)) so the saved bundle does not care which one won.

X is a DataFrame holding at least the calendar columns from features.calendar().
"""
import numpy as np
import pandas as pd
from sklearn.ensemble import HistGradientBoostingRegressor
from sklearn.linear_model import PoissonRegressor

from features import FEATURES_BASE, FEATURES_SEASON


def _group(X, holidays_as_sunday):
    dow = X["dow"].to_numpy()
    if holidays_as_sunday:
        dow = np.where(X["holiday"].to_numpy() == 1, 6, dow)
    return dow * 24 + X["hour"].to_numpy()


class HourOfWeek:
    """B0 / B1: statistic of the target per hour of week (B1: holidays use the Sunday profile).

    quantile=None gives the mean (the existing usage_hour_of_week view), otherwise that empirical quantile.
    """

    def __init__(self, holidays_as_sunday=False, quantile=None):
        self.holidays_as_sunday = holidays_as_sunday
        self.quantile = quantile

    def fit(self, X, y, w=None):
        g = pd.Series(np.asarray(y, dtype=float)).groupby(_group(X, self.holidays_as_sunday))
        stat = g.mean() if self.quantile is None else g.quantile(self.quantile)
        overall = float(np.mean(y)) if self.quantile is None else float(np.quantile(y, self.quantile))
        self.table_ = stat.reindex(range(168), fill_value=overall).to_numpy()
        return self

    def predict(self, X):
        return self.table_[_group(X, self.holidays_as_sunday)]


class PoissonGLM:
    """M1: Poisson GLM on one-hot hour of week (holidays as Sunday), bridge flag and optional season terms."""

    def __init__(self, season=False, alpha=1e-4):
        self.season = season
        self.alpha = alpha

    def _design(self, X):
        g = _group(X, True)
        onehot = np.zeros((len(X), 168))
        onehot[np.arange(len(X)), g] = 1.0
        cols = [onehot, X[["bridge"]].to_numpy(float)]
        if self.season:
            cols.append(X[FEATURES_SEASON].to_numpy(float))
        return np.hstack(cols)

    def fit(self, X, y, w=None):
        self.model_ = PoissonRegressor(alpha=self.alpha, max_iter=1000).fit(self._design(X), y, sample_weight=w)
        return self

    def predict(self, X):
        return self.model_.predict(self._design(X))


class GBM:
    """M2: gradient boosting on calendar features; loss 'poisson' (mean) or 'quantile' (with `quantile`)."""

    def __init__(self, season=False, loss="poisson", quantile=None, max_leaf_nodes=15, max_iter=300,
                 learning_rate=0.05, min_samples_leaf=50, extra=()):
        self.season = season
        self.loss = loss
        self.quantile = quantile
        self.max_leaf_nodes = max_leaf_nodes
        self.max_iter = max_iter
        self.learning_rate = learning_rate
        self.min_samples_leaf = min_samples_leaf
        self.extra = tuple(extra)

    @property
    def columns(self):
        return FEATURES_BASE + (FEATURES_SEASON if self.season else []) + list(self.extra)

    def fit(self, X, y, w=None):
        kw = {"quantile": self.quantile} if self.loss == "quantile" else {}
        self.model_ = HistGradientBoostingRegressor(
            loss=self.loss, max_leaf_nodes=self.max_leaf_nodes, max_iter=self.max_iter,
            learning_rate=self.learning_rate, min_samples_leaf=self.min_samples_leaf,
            categorical_features=[c in ("hour", "dow") for c in self.columns], random_state=0, **kw,
        ).fit(X[self.columns], y, sample_weight=w)
        return self

    def predict(self, X):
        return self.model_.predict(X[self.columns])
