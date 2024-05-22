import tkinter as tk
from tkinter import filedialog
import pandas as pd
import numpy as np
from tabulate import tabulate

def open_file():
    # Abrir a janela de diálogo para selecionar o arquivo CSV
    filename = filedialog.askopenfilename(initialdir="/", title="Select CSV file",
                                           filetypes=(("CSV files", "*.csv"),))
    # Verificar se o usuário selecionou um arquivo
    if filename:
        try:
            # Ler o arquivo CSV usando o pandas
            df = pd.read_csv(filename)
            return df
        except Exception as e:
            # Exibir uma mensagem de erro se ocorrer algum problema ao ler o arquivo
            print(f"Error reading the file: {str(e)}")
    return None

def formatting_file(df):
    try:
        # Encontrar o índice da célula com o texto "EXEMPLES"
        index = df.index[df.iloc[:, 0] == "EXEMPLES"][0]

        # Dividir o DataFrame em dois: ATRIBUTES e EXEMPLES
        df_atributes = df.iloc[:index]
        df_exemples = df.iloc[index:]

        # Criar um vetor de strings usando as linhas da coluna ATRIBUTES do dataframe ATRIBUTES
        EXEMPLES_header = df_atributes.iloc[:, 0].tolist()

        # Atribuir os cabeçalhos do DataFrame EXEMPLES
        df_exemples.columns = ["EXEMPLES"] + list(EXEMPLES_header)

        # Apagar a primeira linha do DataFrame EXEMPLES
        df_exemples = df_exemples.iloc[1:]

        # Substituir os sublinhados "_" por espaços nas strings da coluna "EXEMPLES"
        df_exemples["EXEMPLES"] = df_exemples["EXEMPLES"].str.replace("_", " ")

        # Apagar todas as colunas após o cabeçalho "PREFERENCE" do DataFrame ATRIBUTES
        df_atributes = df_atributes.loc[:, :"PREFERENCE"]

        # Renomear o cabeçalho "ATRIBUTES" para "CRITERIA" no DataFrame df_atributes
        df_atributes = df_atributes.rename(columns={"ATRIBUTES": "CRITERIA"})

        # Adicionar a primeira coluna "ATTRIBUTES" ao DataFrame df_atributes
        df_atributes.insert(0, "ATTRIBUTES", [f"Attribute {i+1}" for i in range(len(df_atributes))])

        # Imprimir os DataFrames resultantes
        print("DataFrame 'ATRIBUTES':")
        print(tabulate(df_atributes, headers='keys', tablefmt='fancy_grid', showindex=False))
        
        # Imprimir as primeiras 20 linhas do DataFrame EXEMPLES
        print("\nDataframe 'EXEMPLES' - Printing the first 20 rows of the dataframe")
        print(tabulate(df_exemples.head(20), headers='keys', tablefmt='fancy_grid', showindex=False))

    except IndexError:
        print("The 'EXEMPLES' cell was not found in the file.")
    return df_atributes, df_exemples

def creating_vectors(df_atributes, df_exemples):
    # Criar os vetores solicitados
    criteria = df_atributes["CRITERIA"].tolist()
    print(criteria)

    data_type = df_atributes["DATA TYPE"].tolist()
    print(data_type)

    preferences = df_atributes["PREFERENCE"].tolist()
    print(preferences)

    decision = df_exemples["Dec"].tolist()
    print(decision)
    
    # Retornar os vetores
    return criteria, data_type, preferences, decision

def union_classes(df_exemples):
    # Verifica se a coluna 'Dec' existe no DataFrame
    if 'Dec' not in df_exemples.columns:
        raise ValueError("A coluna 'Dec' não foi encontrada no DataFrame.")

    # Obtém as diferentes classes de decisão na coluna 'Dec'
    decision_classes = df_exemples['Dec'].unique()
    
    # Cria um dicionário para armazenar as listas de exemplos por classe de decisão
    class_dict = {cls: [] for cls in decision_classes}
    
    # Itera sobre as linhas do DataFrame e adiciona cada linha à lista correspondente no dicionário
    for index, row in df_exemples.iterrows():
        class_dict[row['Dec']].append(row)
    
    # Converte as listas de exemplos em DataFrames
    class_dfs = {cls: pd.DataFrame(rows) for cls, rows in class_dict.items()}
    
    # Imprime a quantidade de exemplos para cada classe
    for cls, df in class_dfs.items():
        print(f"Classe de decisão {cls}: {len(df)} exemplos")
    
    return class_dfs

def downward_union_classes(class_dfs):
    # Obter as classes de decisão em ordem crescente
    decision_classes = sorted(class_dfs.keys())
    
    # Dicionário para armazenar as uniões descendentes
    downward_unions = {}
    
    # Inicializa um DataFrame vazio para agregar as classes
    aggregated_df = pd.DataFrame()
    
    # Itera sobre as classes de decisão da menor para a maior
    for cls in decision_classes:
        # Agrega as classes anteriores e a atual
        aggregated_df = pd.concat([aggregated_df, class_dfs[cls]])
        # Armazena o DataFrame agregado no dicionário
        downward_unions[cls] = aggregated_df.copy()
        # Imprime a quantidade de exemplos na união descendente atual
        print(f"União descendente para classe de decisão {cls}: {len(aggregated_df)} exemplos")
    
    return downward_unions

def upward_union_classes(class_dfs):
    # Obter as classes de decisão em ordem decrescente
    decision_classes = sorted(class_dfs.keys(), reverse=True)
    
    # Dicionário para armazenar as uniões ascendentes
    upward_unions = {}
    
    # Inicializa um DataFrame vazio para agregar as classes
    aggregated_df = pd.DataFrame()
    
    # Itera sobre as classes de decisão da maior para a menor
    for cls in decision_classes:
        # Agrega as classes atuais e posteriores
        aggregated_df = pd.concat([class_dfs[cls], aggregated_df])
        # Armazena o DataFrame agregado no dicionário
        upward_unions[cls] = aggregated_df.copy()
        # Imprime a quantidade de exemplos na união ascendente atual
        print(f"União ascendente para classe de decisão {cls}: {len(aggregated_df)} exemplos")
    
    return upward_unions

def main():
    df = open_file()
    if df is not None:
        df_atributes, df_exemples = formatting_file(df)
        criteria, data_type, preferences, decisions = creating_vectors(df_atributes, df_exemples)
        class_dfs = union_classes(df_exemples)
        
        # Exibir as primeiras linhas de cada DataFrame dividido
        for cls, df in class_dfs.items():
            print(f"Classe de decisão {cls}:")
            print(df.head(), "\n")
        
        # Gerar e exibir as uniões descendentes
        downward_unions = downward_union_classes(class_dfs)
        for cls, union in downward_unions.items():
            print(f"União descendente para classe de decisão {cls}:")
            print(tabulate(union.head(20), headers='keys', tablefmt='fancy_grid', showindex=False))
        
        # Gerar e exibir as uniões ascendentes
        upward_unions = upward_union_classes(class_dfs)
        for cls, union in upward_unions.items():
            print(f"União ascendente para classe de decisão {cls}:")
            print(tabulate(union.head(20), headers='keys', tablefmt='fancy_grid', showindex=False))

# Chamar a função principal
if __name__ == "__main__":
    main()
