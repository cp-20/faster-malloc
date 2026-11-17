window.BENCH_HISTORY = [
  {
    "commit": "round5",
    "message": "Round 5: occupancy-aware placement & runtime profiles (VM matrix)",
    "timestamp": "2026-11-13T08:00:00Z",
    "real_suite": {
      "redis": {
        "glibc": {
          "time_med": 11.31,
          "rss_med_kb": 7373
        },
        "v2_default": {
          "time_med": 11.59,
          "rss_med_kb": 10342
        },
        "jemalloc": {
          "time_med": 11.81,
          "rss_med_kb": 7475
        },
        "mimalloc": {
          "time_med": 12.58,
          "rss_med_kb": 7680
        },
        "tcmalloc": {
          "time_med": 11.63,
          "rss_med_kb": 11469
        }
      },
      "sqlite": {
        "glibc": {
          "time_med": 15.49,
          "rss_med_kb": 11469
        },
        "v2_default": {
          "time_med": 15.88,
          "rss_med_kb": 16998
        },
        "jemalloc": {
          "time_med": 16.06,
          "rss_med_kb": 14643
        },
        "mimalloc": {
          "time_med": 15.69,
          "rss_med_kb": 13210
        },
        "tcmalloc": {
          "time_med": 15.2,
          "rss_med_kb": 19558
        }
      },
      "jq_legacy": {
        "glibc": {
          "time_med": 2.92,
          "rss_med_kb": 378880
        },
        "v2_default": {
          "time_med": 2.87,
          "rss_med_kb": 392192
        },
        "jemalloc": {
          "time_med": 2.61,
          "rss_med_kb": 398336
        },
        "mimalloc": {
          "time_med": 2.59,
          "rss_med_kb": 395264
        },
        "tcmalloc": {
          "time_med": 2.57,
          "rss_med_kb": 386048
        }
      },
      "lua": {
        "glibc": {
          "time_med": 1.02,
          "rss_med_kb": 11776
        },
        "v2_default": {
          "time_med": 0.78,
          "rss_med_kb": 19251
        },
        "jemalloc": {
          "time_med": 0.88,
          "rss_med_kb": 17408
        },
        "mimalloc": {
          "time_med": 0.79,
          "rss_med_kb": 13414
        },
        "tcmalloc": {
          "time_med": 0.9,
          "rss_med_kb": 19763
        }
      },
      "python_legacy": {
        "glibc": {
          "time_med": 6.06,
          "rss_med_kb": 440320
        },
        "v2_default": {
          "time_med": 6.34,
          "rss_med_kb": 505856
        },
        "jemalloc": {
          "time_med": 6.01,
          "rss_med_kb": 461824
        },
        "mimalloc": {
          "time_med": 6.48,
          "rss_med_kb": 467968
        },
        "tcmalloc": {
          "time_med": 5.94,
          "rss_med_kb": 509952
        }
      },
      "node": {
        "glibc": {
          "time_med": 4.11,
          "rss_med_kb": 433152
        },
        "v2_default": {
          "time_med": 4.02,
          "rss_med_kb": 504832
        },
        "jemalloc": {
          "time_med": 4.12,
          "rss_med_kb": 454656
        },
        "mimalloc": {
          "time_med": 4.4,
          "rss_med_kb": 436224
        },
        "tcmalloc": {
          "time_med": 3.88,
          "rss_med_kb": 465920
        }
      },
      "gcc": {
        "glibc": {
          "time_med": 27.84,
          "rss_med_kb": 343040
        },
        "v2_default": {
          "time_med": 27.71,
          "rss_med_kb": 377856
        },
        "jemalloc": {
          "time_med": 26.22,
          "rss_med_kb": 347136
        },
        "mimalloc": {
          "time_med": 25.56,
          "rss_med_kb": 340992
        },
        "tcmalloc": {
          "time_med": 25.7,
          "rss_med_kb": 362496
        }
      },
      "git_legacy": {
        "glibc": {
          "time_med": 1.1,
          "rss_med_kb": 22016
        },
        "v2_default": {
          "time_med": 1.03,
          "rss_med_kb": 27034
        },
        "jemalloc": {
          "time_med": 1.06,
          "rss_med_kb": 26726
        },
        "mimalloc": {
          "time_med": 1.14,
          "rss_med_kb": 23962
        },
        "tcmalloc": {
          "time_med": 1.36,
          "rss_med_kb": 29594
        }
      }
    },
    "cachegrind": {}
  },
  {
    "commit": "round6",
    "message": "Round 6: page-granular purge (MADV_DONTNEED) & idle release",
    "timestamp": "2026-11-13T16:00:00Z",
    "real_suite": {
      "redis": {
        "glibc": {
          "time_med": 5.66,
          "rss_med_kb": 7578
        },
        "v2_default": {
          "time_med": 4.29,
          "rss_med_kb": 10342
        },
        "jemalloc": {
          "time_med": 5.06,
          "rss_med_kb": 7578
        },
        "mimalloc": {
          "time_med": 4.78,
          "rss_med_kb": 7680
        },
        "tcmalloc": {
          "time_med": 3.77,
          "rss_med_kb": 11469
        }
      },
      "sqlite": {
        "glibc": {
          "time_med": 10.67,
          "rss_med_kb": 11366
        },
        "v2_default": {
          "time_med": 11.9,
          "rss_med_kb": 16896
        },
        "jemalloc": {
          "time_med": 11.61,
          "rss_med_kb": 14541
        },
        "mimalloc": {
          "time_med": 10.42,
          "rss_med_kb": 13312
        },
        "tcmalloc": {
          "time_med": 10.9,
          "rss_med_kb": 19763
        }
      },
      "jq_legacy": {
        "glibc": {
          "time_med": 1.65,
          "rss_med_kb": 378880
        },
        "v2_default": {
          "time_med": 1.3,
          "rss_med_kb": 397312
        },
        "jemalloc": {
          "time_med": 1.51,
          "rss_med_kb": 398336
        },
        "mimalloc": {
          "time_med": 1.33,
          "rss_med_kb": 395264
        },
        "tcmalloc": {
          "time_med": 1.32,
          "rss_med_kb": 388096
        }
      },
      "lua": {
        "glibc": {
          "time_med": 0.61,
          "rss_med_kb": 11878
        },
        "v2_default": {
          "time_med": 0.48,
          "rss_med_kb": 19251
        },
        "jemalloc": {
          "time_med": 0.63,
          "rss_med_kb": 17306
        },
        "mimalloc": {
          "time_med": 0.43,
          "rss_med_kb": 13312
        },
        "tcmalloc": {
          "time_med": 0.51,
          "rss_med_kb": 19968
        }
      },
      "python_legacy": {
        "glibc": {
          "time_med": 4.72,
          "rss_med_kb": 440320
        },
        "v2_default": {
          "time_med": 4.91,
          "rss_med_kb": 505856
        },
        "jemalloc": {
          "time_med": 4.76,
          "rss_med_kb": 461824
        },
        "mimalloc": {
          "time_med": 5.02,
          "rss_med_kb": 470016
        },
        "tcmalloc": {
          "time_med": 4.61,
          "rss_med_kb": 508928
        }
      },
      "node": {
        "glibc": {
          "time_med": 1.86,
          "rss_med_kb": 435200
        },
        "v2_default": {
          "time_med": 1.84,
          "rss_med_kb": 504832
        },
        "jemalloc": {
          "time_med": 1.64,
          "rss_med_kb": 453632
        },
        "mimalloc": {
          "time_med": 1.74,
          "rss_med_kb": 437248
        },
        "tcmalloc": {
          "time_med": 1.61,
          "rss_med_kb": 465920
        }
      },
      "gcc": {
        "glibc": {
          "time_med": 15.84,
          "rss_med_kb": 343040
        },
        "v2_default": {
          "time_med": 15.56,
          "rss_med_kb": 377856
        },
        "jemalloc": {
          "time_med": 15.42,
          "rss_med_kb": 348160
        },
        "mimalloc": {
          "time_med": 15.31,
          "rss_med_kb": 343040
        },
        "tcmalloc": {
          "time_med": 15.2,
          "rss_med_kb": 363520
        }
      },
      "git_legacy": {
        "glibc": {
          "time_med": 0.37,
          "rss_med_kb": 21914
        },
        "v2_default": {
          "time_med": 0.33,
          "rss_med_kb": 27034
        },
        "jemalloc": {
          "time_med": 0.35,
          "rss_med_kb": 26624
        },
        "mimalloc": {
          "time_med": 0.32,
          "rss_med_kb": 24166
        },
        "tcmalloc": {
          "time_med": 0.33,
          "rss_med_kb": 28979
        }
      }
    },
    "cachegrind": {}
  },
  {
    "commit": "round10",
    "message": "Round 10: shortlived & footprint profiles (Python/Node RSS -12%)",
    "timestamp": "2026-11-14T12:00:00Z",
    "real_suite": {
      "python_legacy": {
        "glibc": {
          "time_med": 2.711,
          "rss_med_kb": 436932
        },
        "v2_default": {
          "time_med": 2.927,
          "rss_med_kb": 502884
        },
        "v2_footprint": {
          "time_med": 2.847,
          "rss_med_kb": 448828
        },
        "v2_shortlived": {
          "time_med": 2.845,
          "rss_med_kb": 441824
        },
        "jemalloc": {
          "time_med": 2.637,
          "rss_med_kb": 457892
        },
        "mimalloc": {
          "time_med": 2.759,
          "rss_med_kb": 437280
        }
      },
      "node": {
        "glibc": {
          "time_med": 1.159,
          "rss_med_kb": 432124
        },
        "v2_default": {
          "time_med": 1.342,
          "rss_med_kb": 498892
        },
        "v2_footprint": {
          "time_med": 1.289,
          "rss_med_kb": 450492
        },
        "v2_shortlived": {
          "time_med": 1.339,
          "rss_med_kb": 446888
        },
        "jemalloc": {
          "time_med": 1.249,
          "rss_med_kb": 454684
        },
        "mimalloc": {
          "time_med": 1.165,
          "rss_med_kb": 432416
        }
      }
    },
    "cachegrind": {}
  },
  {
    "commit": "1e99694",
    "message": "Pre-optimization baseline (before lazy_bump & flush walk removal)",
    "timestamp": "2026-11-17T03:00:00Z",
    "real_suite": {},
    "cachegrind": {
      "churnmix 50000": {
        "v2_default": {
          "ir": 4073643,
          "d1_misses": 37983,
          "ll_misses": 5226
        }
      },
      "mixed 50000": {
        "v2_default": {
          "ir": 5107865,
          "d1_misses": 118473,
          "ll_misses": 8289
        }
      }
    }
  },
  {
    "commit": "84788a0",
    "message": "Optimize D1/RSS defaults + full GitHub Actions real-app matrix",
    "timestamp": "2026-11-17T04:54:00Z",
    "real_suite": {
      "g++_compile": {
        "glibc": {
          "time_med": 2.617,
          "time_min": 2.597,
          "time_max": 4.995,
          "rss_med_kb": 199272,
          "rss_min_kb": 196964,
          "rss_max_kb": 199412
        },
        "v2_default": {
          "time_med": 2.575,
          "time_min": 2.57,
          "time_max": 2.588,
          "rss_med_kb": 204508,
          "rss_min_kb": 202848,
          "rss_max_kb": 204508
        },
        "v2_footprint": {
          "time_med": 2.645,
          "time_min": 2.635,
          "time_max": 2.689,
          "rss_med_kb": 202592,
          "rss_min_kb": 200792,
          "rss_max_kb": 202788
        },
        "v2_shortlived": {
          "time_med": 2.642,
          "time_min": 2.64,
          "time_max": 2.659,
          "rss_med_kb": 203500,
          "rss_min_kb": 201880,
          "rss_max_kb": 203676
        },
        "v2_throughput": {
          "time_med": 2.582,
          "time_min": 2.56,
          "time_max": 2.586,
          "rss_med_kb": 205308,
          "rss_min_kb": 205248,
          "rss_max_kb": 205436
        },
        "v2_locality": {
          "time_med": 2.604,
          "time_min": 2.589,
          "time_max": 2.614,
          "rss_med_kb": 204696,
          "rss_min_kb": 204592,
          "rss_max_kb": 204756
        },
        "mimalloc": {
          "time_med": 2.597,
          "time_min": 2.582,
          "time_max": 2.611,
          "rss_med_kb": 208676,
          "rss_min_kb": 208620,
          "rss_max_kb": 208832
        },
        "jemalloc": {
          "time_med": 2.578,
          "time_min": 2.572,
          "time_max": 2.605,
          "rss_med_kb": 204440,
          "rss_min_kb": 204332,
          "rss_max_kb": 204456
        },
        "tcmalloc": {
          "time_med": 2.54,
          "time_min": 2.532,
          "time_max": 2.553,
          "rss_med_kb": 209660,
          "rss_min_kb": 209584,
          "rss_max_kb": 209680
        }
      },
      "jq_large": {
        "glibc": {
          "time_med": 0.788,
          "time_min": 0.78,
          "time_max": 0.795,
          "rss_med_kb": 268896,
          "rss_min_kb": 268868,
          "rss_max_kb": 268912
        },
        "v2_default": {
          "time_med": 0.945,
          "time_min": 0.939,
          "time_max": 0.956,
          "rss_med_kb": 264068,
          "rss_min_kb": 263912,
          "rss_max_kb": 264068
        },
        "v2_footprint": {
          "time_med": 1.082,
          "time_min": 1.069,
          "time_max": 1.083,
          "rss_med_kb": 257972,
          "rss_min_kb": 257960,
          "rss_max_kb": 258040
        },
        "v2_shortlived": {
          "time_med": 0.957,
          "time_min": 0.947,
          "time_max": 0.963,
          "rss_med_kb": 257508,
          "rss_min_kb": 257352,
          "rss_max_kb": 257520
        },
        "v2_throughput": {
          "time_med": 0.829,
          "time_min": 0.819,
          "time_max": 0.831,
          "rss_med_kb": 270504,
          "rss_min_kb": 270500,
          "rss_max_kb": 270576
        },
        "v2_locality": {
          "time_med": 0.947,
          "time_min": 0.931,
          "time_max": 0.971,
          "rss_med_kb": 264076,
          "rss_min_kb": 264048,
          "rss_max_kb": 264144
        },
        "mimalloc": {
          "time_med": 0.7,
          "time_min": 0.699,
          "time_max": 0.71,
          "rss_med_kb": 283980,
          "rss_min_kb": 283964,
          "rss_max_kb": 283996
        },
        "jemalloc": {
          "time_med": 0.726,
          "time_min": 0.724,
          "time_max": 0.728,
          "rss_med_kb": 284352,
          "rss_min_kb": 284292,
          "rss_max_kb": 284380
        },
        "tcmalloc": {
          "time_med": 0.724,
          "time_min": 0.721,
          "time_max": 0.729,
          "rss_med_kb": 280848,
          "rss_min_kb": 280804,
          "rss_max_kb": 280888
        }
      },
      "sqlite_oltp": {
        "glibc": {
          "time_med": 0.41,
          "time_min": 0.409,
          "time_max": 0.447,
          "rss_med_kb": 11652,
          "rss_min_kb": 11552,
          "rss_max_kb": 11692
        },
        "v2_default": {
          "time_med": 0.413,
          "time_min": 0.401,
          "time_max": 0.417,
          "rss_med_kb": 13616,
          "rss_min_kb": 13584,
          "rss_max_kb": 13628
        },
        "v2_footprint": {
          "time_med": 0.415,
          "time_min": 0.413,
          "time_max": 0.417,
          "rss_med_kb": 13228,
          "rss_min_kb": 13216,
          "rss_max_kb": 13272
        },
        "v2_shortlived": {
          "time_med": 0.412,
          "time_min": 0.403,
          "time_max": 0.413,
          "rss_med_kb": 13328,
          "rss_min_kb": 13244,
          "rss_max_kb": 13344
        },
        "v2_throughput": {
          "time_med": 0.414,
          "time_min": 0.413,
          "time_max": 0.415,
          "rss_med_kb": 13648,
          "rss_min_kb": 13600,
          "rss_max_kb": 13668
        },
        "v2_locality": {
          "time_med": 0.413,
          "time_min": 0.408,
          "time_max": 0.414,
          "rss_med_kb": 13628,
          "rss_min_kb": 13600,
          "rss_max_kb": 13628
        },
        "mimalloc": {
          "time_med": 0.414,
          "time_min": 0.406,
          "time_max": 0.415,
          "rss_med_kb": 13836,
          "rss_min_kb": 13804,
          "rss_max_kb": 13868
        },
        "jemalloc": {
          "time_med": 0.412,
          "time_min": 0.398,
          "time_max": 0.412,
          "rss_med_kb": 14452,
          "rss_min_kb": 14448,
          "rss_max_kb": 14452
        },
        "tcmalloc": {
          "time_med": 0.415,
          "time_min": 0.415,
          "time_max": 0.415,
          "rss_med_kb": 18172,
          "rss_min_kb": 18164,
          "rss_max_kb": 18212
        }
      },
      "python_data": {
        "glibc": {
          "time_med": 0.263,
          "time_min": 0.259,
          "time_max": 0.263,
          "rss_med_kb": 145188,
          "rss_min_kb": 145044,
          "rss_max_kb": 145208
        },
        "v2_default": {
          "time_med": 0.284,
          "time_min": 0.282,
          "time_max": 0.29,
          "rss_med_kb": 154896,
          "rss_min_kb": 154884,
          "rss_max_kb": 154908
        },
        "v2_footprint": {
          "time_med": 0.284,
          "time_min": 0.283,
          "time_max": 0.29,
          "rss_med_kb": 150184,
          "rss_min_kb": 150136,
          "rss_max_kb": 150208
        },
        "v2_shortlived": {
          "time_med": 0.288,
          "time_min": 0.284,
          "time_max": 0.291,
          "rss_med_kb": 150316,
          "rss_min_kb": 150308,
          "rss_max_kb": 150408
        },
        "v2_throughput": {
          "time_med": 0.289,
          "time_min": 0.284,
          "time_max": 0.289,
          "rss_med_kb": 192512,
          "rss_min_kb": 192484,
          "rss_max_kb": 192544
        },
        "v2_locality": {
          "time_med": 0.283,
          "time_min": 0.281,
          "time_max": 0.285,
          "rss_med_kb": 154892,
          "rss_min_kb": 154872,
          "rss_max_kb": 154900
        },
        "mimalloc": {
          "time_med": 0.276,
          "time_min": 0.265,
          "time_max": 0.282,
          "rss_med_kb": 165276,
          "rss_min_kb": 165244,
          "rss_max_kb": 165288
        },
        "jemalloc": {
          "time_med": 0.274,
          "time_min": 0.267,
          "time_max": 0.28,
          "rss_med_kb": 164464,
          "rss_min_kb": 161856,
          "rss_max_kb": 164464
        },
        "tcmalloc": {
          "time_med": 0.272,
          "time_min": 0.272,
          "time_max": 0.274,
          "rss_med_kb": 158612,
          "rss_min_kb": 158540,
          "rss_max_kb": 158640
        }
      },
      "git_revlist": {
        "glibc": {
          "time_med": 0.016,
          "time_min": 0.016,
          "time_max": 0.017,
          "rss_med_kb": 11648,
          "rss_min_kb": 11608,
          "rss_max_kb": 11660
        },
        "v2_default": {
          "time_med": 0.017,
          "time_min": 0.017,
          "time_max": 0.018,
          "rss_med_kb": 13512,
          "rss_min_kb": 13460,
          "rss_max_kb": 13540
        },
        "v2_footprint": {
          "time_med": 0.017,
          "time_min": 0.017,
          "time_max": 0.018,
          "rss_med_kb": 13244,
          "rss_min_kb": 13216,
          "rss_max_kb": 13272
        },
        "v2_shortlived": {
          "time_med": 0.017,
          "time_min": 0.017,
          "time_max": 0.017,
          "rss_med_kb": 13284,
          "rss_min_kb": 13200,
          "rss_max_kb": 13284
        },
        "v2_throughput": {
          "time_med": 0.017,
          "time_min": 0.017,
          "time_max": 0.018,
          "rss_med_kb": 13472,
          "rss_min_kb": 13456,
          "rss_max_kb": 13540
        },
        "v2_locality": {
          "time_med": 0.017,
          "time_min": 0.017,
          "time_max": 0.017,
          "rss_med_kb": 13528,
          "rss_min_kb": 13464,
          "rss_max_kb": 13540
        },
        "mimalloc": {
          "time_med": 0.017,
          "time_min": 0.017,
          "time_max": 0.018,
          "rss_med_kb": 13868,
          "rss_min_kb": 13848,
          "rss_max_kb": 13868
        },
        "jemalloc": {
          "time_med": 0.019,
          "time_min": 0.019,
          "time_max": 0.019,
          "rss_med_kb": 14444,
          "rss_min_kb": 14408,
          "rss_max_kb": 14444
        },
        "tcmalloc": {
          "time_med": 0.023,
          "time_min": 0.023,
          "time_max": 0.024,
          "rss_med_kb": 18016,
          "rss_min_kb": 18000,
          "rss_max_kb": 18040
        }
      },
      "xz_mt2": {
        "glibc": {
          "time_med": 2.225,
          "time_min": 2.018,
          "time_max": 2.288,
          "rss_med_kb": 57276,
          "rss_min_kb": 57272,
          "rss_max_kb": 57340
        },
        "v2_default": {
          "time_med": 2.218,
          "time_min": 2.163,
          "time_max": 2.324,
          "rss_med_kb": 57700,
          "rss_min_kb": 57584,
          "rss_max_kb": 57772
        },
        "v2_footprint": {
          "time_med": 2.029,
          "time_min": 1.985,
          "time_max": 2.403,
          "rss_med_kb": 57708,
          "rss_min_kb": 57648,
          "rss_max_kb": 57712
        },
        "v2_shortlived": {
          "time_med": 2.069,
          "time_min": 2.009,
          "time_max": 2.372,
          "rss_med_kb": 57772,
          "rss_min_kb": 57708,
          "rss_max_kb": 57776
        },
        "v2_throughput": {
          "time_med": 2.072,
          "time_min": 2.025,
          "time_max": 2.333,
          "rss_med_kb": 57712,
          "rss_min_kb": 57712,
          "rss_max_kb": 57712
        },
        "v2_locality": {
          "time_med": 2.12,
          "time_min": 2.026,
          "time_max": 2.243,
          "rss_med_kb": 57840,
          "rss_min_kb": 57840,
          "rss_max_kb": 57840
        },
        "mimalloc": {
          "time_med": 2.075,
          "time_min": 1.947,
          "time_max": 2.196,
          "rss_med_kb": 58252,
          "rss_min_kb": 58244,
          "rss_max_kb": 58252
        },
        "jemalloc": {
          "time_med": 2.153,
          "time_min": 2.078,
          "time_max": 2.18,
          "rss_med_kb": 60968,
          "rss_min_kb": 60680,
          "rss_max_kb": 61096
        },
        "tcmalloc": {
          "time_med": 2.151,
          "time_min": 2.022,
          "time_max": 2.263,
          "rss_med_kb": 69440,
          "rss_min_kb": 69268,
          "rss_max_kb": 69440
        }
      },
      "sort_p4": {
        "glibc": {
          "time_med": 0.122,
          "time_min": 0.122,
          "time_max": 0.123,
          "rss_med_kb": 33580,
          "rss_min_kb": 33488,
          "rss_max_kb": 33656
        },
        "v2_default": {
          "time_med": 0.131,
          "time_min": 0.129,
          "time_max": 0.131,
          "rss_med_kb": 35040,
          "rss_min_kb": 34772,
          "rss_max_kb": 35164
        },
        "v2_footprint": {
          "time_med": 0.129,
          "time_min": 0.128,
          "time_max": 0.132,
          "rss_med_kb": 34588,
          "rss_min_kb": 34536,
          "rss_max_kb": 34936
        },
        "v2_shortlived": {
          "time_med": 0.129,
          "time_min": 0.128,
          "time_max": 0.129,
          "rss_med_kb": 35108,
          "rss_min_kb": 35080,
          "rss_max_kb": 35332
        },
        "v2_throughput": {
          "time_med": 0.129,
          "time_min": 0.127,
          "time_max": 0.13,
          "rss_med_kb": 43868,
          "rss_min_kb": 43688,
          "rss_max_kb": 43920
        },
        "v2_locality": {
          "time_med": 0.128,
          "time_min": 0.128,
          "time_max": 0.129,
          "rss_med_kb": 35252,
          "rss_min_kb": 35068,
          "rss_max_kb": 35320
        },
        "mimalloc": {
          "time_med": 0.131,
          "time_min": 0.128,
          "time_max": 0.134,
          "rss_med_kb": 51728,
          "rss_min_kb": 51488,
          "rss_max_kb": 51892
        },
        "jemalloc": {
          "time_med": 0.132,
          "time_min": 0.13,
          "time_max": 0.133,
          "rss_med_kb": 41364,
          "rss_min_kb": 41176,
          "rss_max_kb": 41444
        },
        "tcmalloc": {
          "time_med": 0.127,
          "time_min": 0.127,
          "time_max": 0.129,
          "rss_med_kb": 44428,
          "rss_min_kb": 44420,
          "rss_max_kb": 44560
        }
      }
    },
    "cachegrind": {
      "churnmix 50000": {
        "v2_default": {
          "ir": 3957445,
          "d1_misses": 4409,
          "ll_misses": 5041
        },
        "v2_locality": {
          "ir": 3958657,
          "d1_misses": 4414,
          "ll_misses": 5057
        }
      },
      "mixed 50000": {
        "v2_default": {
          "ir": 5053774,
          "d1_misses": 67270,
          "ll_misses": 7740
        },
        "v2_locality": {
          "ir": 5395978,
          "d1_misses": 90954,
          "ll_misses": 7755
        }
      }
    }
  }
];
