# libos32ecs — Entity Component System (ECS) オブジェクト管理

ゲーム内のキャラクターやオブジェクトのデータ構造化、更新順序、ライフサイクル（生成・遅延破棄・再利用）を、メモリ効率のよい SoA（Structure of Arrays）形式で静的・一元管理する軽量ECSフレームワークです。

詳細な設計思想、API仕様、実装状況については [DESIGN.md](DESIGN.md) を参照してください。
